/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "AutoRemoteSyscalls.h"

#include <limits.h>
#include <linux/net.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sstream>

#include "rr/rr.h"

#include "RecordSession.h"
#include "RecordTask.h"
#include "ReplaySession.h"
#include "Session.h"
#include "Task.h"
#include "core.h"
#include "kernel_abi.h"
#include "kernel_metadata.h"
#include "log.h"
#include "record_signal.h"
#include "util.h"

using namespace std;

namespace rr {

/**
 * The ABI of the socketcall syscall is a nightmare; the first arg to
 * the kernel is the sub-operation, and the second argument is a
 * pointer to the args.  The args depend on the sub-op.
 */
template <typename Arch> struct socketcall_args {
  typename Arch::signed_long args[3];
} __attribute__((packed));

void AutoRestoreMem::init(const void* mem, ssize_t num_bytes) {
  ASSERT(remote.task(),
         remote.enable_mem_params() == AutoRemoteSyscalls::ENABLE_MEMORY_PARAMS)
      << "Memory parameters were disabled";

  len = num_bytes;
  saved_sp = remote.regs().sp();

  remote.regs().set_sp(remote.regs().sp() - len);
  remote.task()->set_regs(remote.regs());
  if (remote.task()->is_exiting()) {
    // Leave addr == nullptr
    return;
  }

  addr = remote.regs().sp();

  data.resize(len);
  bool ok = true;
  remote.task()->read_bytes_helper(addr, len, data.data(), &ok);
  if (mem) {
    remote.task()->write_bytes_helper(addr, len, mem, &ok);
  }
  if (!ok) {
    addr = nullptr;
  }
}

AutoRestoreMem::~AutoRestoreMem() {
  DEBUG_ASSERT(saved_sp == remote.regs().sp() + len);

  if (addr) {
    // XXX what should we do if this task was sigkilled but the address
    // space is used by other live tasks?
    remote.task()->write_bytes_helper(addr, len, data.data());
  }
  remote.regs().set_sp(remote.regs().sp() + len);
  remote.task()->set_regs(remote.regs());
}

static bool is_SIGTRAP_default_and_unblocked(Task* t) {
  if (!t->session().is_recording()) {
    return true;
  }
  RecordTask* rt = static_cast<RecordTask*>(t);
  return rt->sig_disposition(SIGTRAP) == SIGNAL_DEFAULT &&
         !rt->is_sig_blocked(SIGTRAP);
}

AutoRemoteSyscalls::AutoRemoteSyscalls(Task* t,
                                       MemParamsEnabled enable_mem_params)
    : t(t),
      initial_regs(t->regs()),
      initial_ip(t->ip()),
      initial_sp(t->regs().sp()),
      initial_at_seccomp(t->ptrace_event() == PTRACE_EVENT_SECCOMP),
      restore_wait_status(t->status()),
      new_tid_(-1),
      scratch_mem_was_mapped(false),
      use_singlestep_path(false),
      enable_mem_params_(enable_mem_params),
      restore_sigmask(false),
      need_sigpending_renable(false),
      need_desched_event_reenable(false) {
  if (initial_at_seccomp) {
    // This should only ever happen during recording - we don't use the
    // seccomp traps during replay.
    ASSERT(t, t->session().is_recording());
  }
  // We support two paths for syscalls:
  // -- a fast path using a privileged untraced syscall and PTRACE_SINGLESTEP.
  // This only requires a single task-wait.
  // -- a slower path using a privileged traced syscall and PTRACE_SYSCALL/
  // PTRACE_CONT via Task::enter_syscall(). This requires 2 or 3 task-waits
  // depending on whether the seccomp event fires before the syscall-entry
  // event.
  // Use the slow path when running under rr, because the rr recording us
  // needs to see and trace these tracee syscalls, and if they're untraced by
  // us they're also untraced by the outer rr.
  // Use the slow path if SIGTRAP is blocked or ignored because otherwise
  // the PTRACE_SINGLESTEP will cause the kernel to unblock it.
  setup_path(t->vm()->has_rr_page() && !running_under_rr() &&
             is_SIGTRAP_default_and_unblocked(t));
  if (enable_mem_params == ENABLE_MEMORY_PARAMS) {
    maybe_fix_stack_pointer();
  }
  if (t->status().is_syscall() && t->regs().syscall_may_restart()) {
    // VERY rare corner case alert: It is possible for the following sequence
    // of events to occur:
    //
    // 1. Thread A is in a blocking may-restart syscall and gets interrupted by a tg-targeted signal
    // 2. Thread B dequeues the signal
    // 3. Thread A is in the syscall-exit-stop with TIF_SIGPENDING set (with registers indicating syscall restart)
    // 4. We get here to perform an AutoRemoteSyscall
    // 5. During AutoRemoteSyscall, TIF_SIGPENDING gets cleared on return to userspace
    // 6. We finish the AutoRemoteSyscall and re-apply the registers.
    // 7. ... As a result, the kernel does not check whether it needs to perform the
    ///   syscall-restart register adjustment because TIF_SIGPENDING is not set.
    // 8. The -ERESTART error code leaks to userspace.
    //
    // Arguably this is a kernel bug, but it's not clear how the behavior should be changed.
    //
    // To work around this, we forcibly re-enable TIF_SIGPENDING when cleaning up
    // AutoRemoteSyscall (see below).
    need_sigpending_renable = true;
  }
  if (t->session().is_recording()) {
    RecordTask *rt = static_cast<RecordTask*>(t);
    sig_set_t signals_to_block = 0;

    if (rt->schedule_frozen) {
      // If we're explicitly controlling the schedule, make sure not to accidentally run
      // any signals that we were not meant to be able to see.
      memset(&signals_to_block, 0xff, sizeof(sig_set_t));
    }
    if (desched_event_armed(rt)) {
      // If the desched event is enabled, we need to disable it, so that we don't get
      // the desched signal interrupting the syscall we're trying to make. We also
      // need to mask it, so that if there's a pending desched signal from before
      // we disable it, we don't accidently steal it.
      signals_to_block |= signal_bit(rt->session().syscallbuf_desched_sig());
      need_desched_event_reenable = true;
      disarm_desched_event(rt);
    }

    if (signals_to_block) {
      restore_sigmask = true;
      sigmask_to_restore = rt->get_sigmask();
      // Ignore the process dying here - we'll notice later.
      (void)rt->set_sigmask(signals_to_block | sigmask_to_restore);
    }
  }
}

void AutoRemoteSyscalls::setup_path(bool enable_singlestep_path) {
#if defined(__aarch64__)
  // XXXkhuey this fast path doesn't work on AArch64 yet, go slow instead
  enable_singlestep_path = false;
#endif

  if (!replaced_bytes.empty()) {
    // XXX what to do here to clean up if the task died unexpectedly?
    t->write_mem(remote_ptr<uint8_t>(initial_regs.ip().to_data_ptr<uint8_t>()),
                 replaced_bytes.data(), replaced_bytes.size());
  }

  remote_code_ptr syscall_ip;
  use_singlestep_path = enable_singlestep_path;
  if (use_singlestep_path) {
    syscall_ip = AddressSpace::rr_page_syscall_entry_point(
        AddressSpace::UNTRACED, AddressSpace::PRIVILEGED,
        AddressSpace::RECORDING_AND_REPLAY, t->arch());
  } else {
    syscall_ip = t->vm()->traced_syscall_ip();
  }
  initial_regs.set_ip(syscall_ip);

  // We need to make sure to clear any breakpoints or other alterations of
  // the syscall instruction we're using. Note that the tracee may have set its
  // own breakpoints or otherwise modified the instruction, so suspending our
  // own breakpoint is insufficient.
  std::vector<uint8_t> syscall = rr::syscall_instruction(t->arch());
  bool ok = true;
  replaced_bytes =
      t->read_mem(initial_regs.ip().to_data_ptr<uint8_t>(), syscall.size(), &ok);
  if (!ok) {
    // The task died
    return;
  }
  if (replaced_bytes == syscall) {
    replaced_bytes.clear();
  } else {
    t->write_mem(initial_regs.ip().to_data_ptr<uint8_t>(), syscall.data(),
                 syscall.size(), &ok);
  }
}

static bool is_usable_area(const KernelMapping& km) {
  return (km.prot() & (PROT_READ | PROT_WRITE)) == (PROT_READ | PROT_WRITE) &&
         (km.flags() & MAP_PRIVATE);
}

void AutoRemoteSyscalls::maybe_fix_stack_pointer() {
  if (!t->session().done_initial_exec()) {
    return;
  }

  remote_ptr<void> last_stack_byte = t->regs().sp() - 1;
  if (t->vm()->has_mapping(last_stack_byte)) {
    auto m = t->vm()->mapping_of(last_stack_byte);
    if (is_usable_area(m.map) && m.map.start() + 2048 <= t->regs().sp()) {
      // 'sp' is in a stack region and there's plenty of space there. No need
      // to fix anything.
      return;
    }
  }

  MemoryRange found_stack;
  for (const auto& m : t->vm()->maps()) {
    if (is_usable_area(m.map)) {
      found_stack = m.map;
      break;
    }
  };

  if (found_stack.start().is_null()) {
    AutoRemoteSyscalls remote(t, DISABLE_MEMORY_PARAMS);
    found_stack =
        MemoryRange(remote.infallible_mmap_syscall_if_alive(
                        remote_ptr<void>(), 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
                    4096);
    ASSERT(t, !found_stack.start().is_null())
      << "Tracee unexpectedly died here";
    scratch_mem_was_mapped = true;
  }

  fixed_sp = found_stack.end();
  DEBUG_ASSERT(!fixed_sp.is_null());
  initial_regs.set_sp(fixed_sp);
}

AutoRemoteSyscalls::~AutoRemoteSyscalls() { restore_state_to(t); }

void AutoRemoteSyscalls::restore_state_to(Task* t) {
  // Check if the task was unexpectedly killed via SIGKILL or equivalent.
  bool is_exiting = !t->is_stopped() || t->ptrace_event() == PTRACE_EVENT_EXIT ||
    t->was_reaped();

  // Unmap our scatch region if required
  if (scratch_mem_was_mapped && !is_exiting) {
    AutoRemoteSyscalls remote(t, DISABLE_MEMORY_PARAMS);
    remote.infallible_syscall(syscall_number_for_munmap(arch()),
                              fixed_sp - 4096, 4096);
  }
  if (!replaced_bytes.empty()) {
    // XXX how to clean up if the task died and the address space is shared with live task?
    t->write_mem(remote_ptr<uint8_t>(initial_regs.ip().to_data_ptr<uint8_t>()),
                 replaced_bytes.data(), replaced_bytes.size());
  }
  auto regs = initial_regs;
  regs.set_ip(initial_ip);
  regs.set_sp(initial_sp);
  if (is_exiting) {
    // Don't restore status; callers need to see the task is exiting.
    // And the other stuff we don't below won't work.
    // But do restore registers so it looks like the exit happened in a clean state.
    t->set_regs(regs);
    return;
  }

  if (t->arch() == aarch64 && regs.syscall_may_restart()) {
    // On AArch64, the kernel restarts aborted syscalls using an internal `orig_x0`.
    // This gets overwritten everytime we make a syscall so we need to restore it
    // if we are at a syscall that may restart.
    // The kernel `orig_x0` isn't accessible from ptrace AFAICT but fortunately
    // it does **NOT** get reset on syscall exit so we can actually set it's value
    // just by making a dummy syscall with the correct x0 value.
    auto restart_res = regs.syscall_result();
    regs.set_ip(t->vm()->traced_syscall_ip());
    // This can be any side-effect-free syscall that doesn't care about arg1.
    // The kernel sets its `orig_x0` no matter whether the syscall actually needs it.
    regs.set_syscallno(rr::ARM64Arch::getpid);
    regs.set_arg1(regs.orig_arg1());
    t->set_regs(regs);
    if (t->enter_syscall(true)) {
      if (!t->resume_execution(RESUME_SYSCALL, RESUME_WAIT_NO_EXIT, RESUME_NO_TICKS)) {
        // Tracee died unexpectedly, there is nothing more we can do.
        // Do not restore the status, we want callers to see that the task died.
        return;
      }
    }
    regs.set_ip(initial_ip);
    regs.set_syscallno(regs.original_syscallno());
    regs.set_syscall_result(restart_res);
  }
  // If we were sitting at a seccomp trap, try to get back there by resuming
  // here. Since the original register contents caused a seccomp trap,
  // re-running the syscall with the same registers should put us right back
  // to this same seccomp trap.
  if (initial_at_seccomp && t->ptrace_event() != PTRACE_EVENT_SECCOMP) {
    regs.set_ip(initial_ip.decrement_by_syscall_insn_length(t->arch()));
    regs.set_syscallno(regs.original_syscallno());
    t->set_regs(regs);
    RecordTask* rt = static_cast<RecordTask*>(t);
    while (true) {
      if (!rt->resume_execution(RESUME_CONT, RESUME_WAIT_NO_EXIT, RESUME_NO_TICKS)) {
        // Tracee died unexpectedly, there is nothing more we can do.
        // Do not restore the status, we want callers to see that the task died.
        return;
      }
      if (rt->ptrace_event())
        break;
      rt->stash_sig();
    }
    ASSERT(rt, rt->ptrace_event() == PTRACE_EVENT_SECCOMP);
  } else {
    // Restore stomped registers.
    t->set_regs(regs);
  }
  t->set_status(restore_wait_status);
  if (restore_sigmask) {
    static_cast<RecordTask*>(t)->set_sigmask(sigmask_to_restore);
  }
  if (need_desched_event_reenable) {
    arm_desched_event(static_cast<RecordTask*>(t));
  }
  if (need_sigpending_renable) {
    // The purpose of this PTRACE_INTERRUPT is to re-enable TIF_SIGPENDING on
    // the tracee, without forcing any actual signals on it. Since PTRACE_INTERRUPT
    // needs to be able to interrupt re-startable system calls, it is required
    // to set TIF_SIGPENDING, but the fact that this works is of course a very
    // deep implementation detail.
    // If this fails then the tracee must be dead or no longer traced, in which
    // case we no longer care about its TIF_SIGPENDING status.
    t->do_ptrace_interrupt();
  }
}

static bool ignore_signal(Task* t) {
  int sig = t->stop_sig();
  if (!sig) {
    return false;
  }
  if (t->session().is_replaying()) {
    if (ReplaySession::is_ignored_signal(sig)) {
      return true;
    }
  } else if (t->session().is_recording()) {
    auto rt = static_cast<RecordTask*>(t);
    if (sig != rt->session().syscallbuf_desched_sig()) {
      rt->stash_sig();
    }
    return true;
  }
  siginfo_t siginfo;
  errno = 0;
  t->fallible_ptrace(PTRACE_GETSIGINFO, nullptr, &siginfo);
  if (errno) {
    ASSERT(t, false) << "Unexpected signal " << signal_name(sig);
  } else {
    ASSERT(t, false) << "Unexpected signal " << siginfo;
  }
  return false;
}

long AutoRemoteSyscalls::syscall_base(int syscallno, Registers& callregs) {
  LOG(debug) << "syscall " << syscall_name(syscallno, t->arch()) << " " << callregs;

  if (t->is_exiting()) {
    LOG(debug) << "Task is dying, don't try anything.";
    ASSERT(t, t->stopped_or_unexpected_exit()) << "Already seen exit event";
    return -ESRCH;
  }

  if ((int)callregs.arg1() == SIGTRAP && use_singlestep_path &&
      (is_sigaction_syscall(syscallno, t->arch()) ||
       is_rt_sigaction_syscall(syscallno, t->arch()) ||
       is_signal_syscall(syscallno, t->arch()))) {
    // Don't use the fast path if we're about to set up a signal handler
    // for SIGTRAP!
    LOG(debug) << "Disabling singlestep path due to SIGTRAP sigaction";
    setup_path(false);
    callregs.set_ip(initial_regs.ip());
  }

  callregs.set_original_syscallno(syscallno);
  callregs.set_syscallno(syscallno);
  t->set_regs(callregs);

  bool from_seccomp = initial_at_seccomp && t->ptrace_event() == PTRACE_EVENT_SECCOMP;
  if (use_singlestep_path && !from_seccomp) {
    while (true) {
      if (!t->resume_execution(RESUME_SINGLESTEP, RESUME_WAIT_NO_EXIT, RESUME_NO_TICKS)) {
        // Tracee was killed, there is nothing more we can do.
        ASSERT(t, t->stopped_or_unexpected_exit()) << "Couldn't singlestep";
        return -ESRCH;
      }
      LOG(debug) << "Used singlestep path; status=" << t->status();
      // When a PTRACE_EVENT_EXIT is returned we don't update registers
      if (t->ip() != callregs.ip()) {
        // We entered the syscall, so stop now
        break;
      }
      if (t->stop_sig() == SIGTRAP && t->get_siginfo().si_code == TRAP_TRACE) {
        // On aarch64, if we were previously in a syscall-exit stop, continuing
        // with PTRACE_SINGLESTEP will result in incurring a trap upon execution
        // of the first instruction in userspace. Ignore such a trap.
        continue;
      }
      if (ignore_signal(t)) {
        // We were interrupted by a signal before we even entered the syscall
        continue;
      }
      ASSERT(t, false) << "Unexpected status " << t->status();
    }
  } else {
    if (from_seccomp) {
      LOG(debug) << "Skipping enter_syscall - already at seccomp stop";
    } else {
      if (!t->enter_syscall(true)) {
        // Tracee was killed, there is nothing more we can do.
        // Ensure callers see the task death status.
        ASSERT(t, t->stopped_or_unexpected_exit()) << "couldn't enter syscall";
        return -ESRCH;
      }
      LOG(debug) << "Used enter_syscall; status=" << t->status();
    }
    if (!t->resume_execution(RESUME_SYSCALL, RESUME_WAIT_NO_EXIT, RESUME_NO_TICKS)) {
      // Tracee was killed, there is nothing more we can do.
      // Ensure callers see the task death status.
      ASSERT(t, t->stopped_or_unexpected_exit()) << "couldn't resume syscall";
      return -ESRCH;
    }
    LOG(debug) << "syscall exit status=" << t->status();
  }
  while (true) {
    if (t->status().is_syscall() ||
        (t->stop_sig() == SIGTRAP &&
         is_kernel_trap(t->get_siginfo().si_code))) {
      // If we got a SIGTRAP then we assume that's our singlestep and we're
      // done.
      break;
    }
    if (is_clone_syscall(syscallno, t->arch()) &&
        t->clone_syscall_is_complete(&new_tid_, t->arch())) {
      if (!t->resume_execution(RESUME_SYSCALL, RESUME_WAIT_NO_EXIT, RESUME_NO_TICKS)) {
        // Tracee was killed, there is nothing more we can do.
        ASSERT(t, t->stopped_or_unexpected_exit()) << "Couldn't resume clone";
        return -ESRCH;
      }
      LOG(debug) << "got clone event; new status=" << t->status();
      continue;
    }
    if (ignore_signal(t)) {
      if (t->regs().syscall_may_restart()) {
        if (!t->enter_syscall(true)) {
          // Tracee was killed, there is nothing more we can do.
          ASSERT(t, t->stopped_or_unexpected_exit()) << "Couldn't restart";
          return -ESRCH;
        }
        LOG(debug) << "signal ignored; restarting syscall, status="
                   << t->status();
        if (!t->resume_execution(RESUME_SYSCALL, RESUME_WAIT_NO_EXIT, RESUME_NO_TICKS)) {
          // Tracee was killed, there is nothing more we can do.
          ASSERT(t, t->stopped_or_unexpected_exit()) << "Couldn't resume restart";
          return -ESRCH;
        }
        LOG(debug) << "syscall exit status=" << t->status();
        continue;
      }
      LOG(debug) << "signal ignored";
      // We have been notified of a signal after a non-interruptible syscall
      // completed. Don't continue, we're done here.
      break;
    }
    ASSERT(t, false) << "Unexpected status " << t->status();
    break;
  }

  LOG(debug) << "done, result=" << t->regs().syscall_result();
  return t->regs().syscall_result();
}

SupportedArch AutoRemoteSyscalls::arch() const { return t->arch(); }

template <typename Arch>
static void write_socketcall_args(Task* t, remote_ptr<void> remote_mem,
                                  typename Arch::signed_long arg1,
                                  typename Arch::signed_long arg2,
                                  typename Arch::signed_long arg3,
                                  bool* ok) {
  socketcall_args<Arch> sc_args = { { arg1, arg2, arg3 } };
  t->write_mem(remote_mem.cast<socketcall_args<Arch>>(), sc_args, ok);
}

template <typename Arch>
struct fd_message {
  // Unfortunately we need to send at least one byte of data in our
  // message for it to work
  char data;
  typename Arch::iovec msgdata;
  char cmsgbuf[Arch::cmsg_space(sizeof(int))];
  typename Arch::msghdr msg;
  // XXX: Could make this conditional on Arch
  socketcall_args<Arch> socketcall;
  void init(remote_ptr<fd_message<Arch>> base) {
    data = 0;
    msgdata.iov_base = REMOTE_PTR_FIELD(base, data);
    msgdata.iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_control = REMOTE_PTR_FIELD(base, cmsgbuf);
    msg.msg_controllen = sizeof(cmsgbuf);
    msg.msg_iov = REMOTE_PTR_FIELD(base, msgdata);
    msg.msg_iovlen = 1;
  }
  fd_message(remote_ptr<fd_message<Arch>> base) {
    init(base);
  }
  fd_message() {
    init((uintptr_t)this);
  }
  remote_ptr<fd_message<Arch>> remote_this() {
    return msgdata.iov_base.rptr().as_int();
  }
  remote_ptr<typename Arch::msghdr> remote_msg() {
    return REMOTE_PTR_FIELD(remote_this(), msg);
  }
  remote_ptr<socketcall_args<Arch>> remote_sc_args() {
    return REMOTE_PTR_FIELD(remote_this(), socketcall);
  }
  remote_ptr<int> remote_cmsgdata() {
    return REMOTE_PTR_FIELD(remote_this(), cmsgbuf).as_int() +
      sizeof(typename Arch::cmsghdr);
  }
};

template <typename Arch>
static long child_sendmsg(AutoRemoteSyscalls& remote, int child_sock, int fd) {
  AutoRestoreMem remote_buf(remote, nullptr, sizeof(fd_message<Arch>));
  fd_message<Arch> msg(remote_buf.get().cast<fd_message<Arch>>());
  // Pull the puppet strings to have the child send its fd
  // to us.  Similarly to above, we DONT_WAIT on the
  // call to finish, since it's likely not defined whether the
  // sendmsg() may block on our recvmsg()ing what the tracee
  // sent us (in which case we would deadlock with the tracee).
  // We call sendmsg on child socket, but first we have to prepare a lot of
  // data.
  auto cmsg = reinterpret_cast<typename Arch::cmsghdr*>(msg.cmsgbuf);
  cmsg->cmsg_len = Arch::cmsg_len(sizeof(fd));
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  *static_cast<int*>(Arch::cmsg_data(cmsg)) = fd;

  if (has_socketcall_syscall(Arch::arch())) {
    socketcall_args<Arch> sc_args = { { child_sock, (typename Arch::signed_long)msg.remote_msg().as_int(), 0 } };
    msg.socketcall = sc_args;
  }

  bool ok = true;
  remote.task()->write_bytes_helper(remote_buf.get().cast<char>(),
    sizeof(msg), &msg, &ok);

  if (!ok) {
    return -ESRCH;
  }
  if (!has_socketcall_syscall(Arch::arch())) {
    return remote.syscall(Arch::sendmsg, child_sock, msg.remote_msg(), 0);
  }
  return remote.syscall(Arch::socketcall, SYS_SENDMSG, msg.remote_sc_args());
}

template <typename Arch>
static long child_recvmsg(AutoRemoteSyscalls& remote, int child_sock) {
  AutoRestoreMem remote_buf(remote, nullptr, sizeof(fd_message<Arch>));
  fd_message<Arch> msg(remote_buf.get().cast<fd_message<Arch>>());
  bool ok = true;

  if (has_socketcall_syscall(Arch::arch())) {
    socketcall_args<Arch> sc_args = { { child_sock,
      (typename Arch::signed_long)msg.remote_msg().as_int(), 0 } };
    msg.socketcall = sc_args;
  }

  remote.task()->write_bytes_helper(remote_buf.get().cast<char>(),
    sizeof(msg), &msg, &ok);

  if (!ok) {
    ASSERT(remote.task(), errno == ESRCH || errno == EIO)
        << "Error writing " << remote_buf.get() << " in " << remote.task()->tid;
    LOG(debug) << "Failed to write memory";
    return -ESRCH;
  }
  int ret = 0;
  if (has_socketcall_syscall(Arch::arch())) {
    ret = remote.syscall(Arch::socketcall, SYS_RECVMSG, msg.remote_sc_args());
  } else {
    ret = remote.syscall(Arch::recvmsg, child_sock, msg.remote_msg(), 0);
  }
  if (ret < 0) {
    LOG(debug) << "Failed to recvmsg " << ret;
    return ret;
  }

  typename Arch::msghdr msghdr =
      remote.task()->read_mem(msg.remote_msg(), &ok);
  if (!ok) {
    ASSERT(remote.task(), errno == ESRCH || errno == EIO);
    LOG(debug) << "Failed to read msghdr";
    return -ESRCH;
  }
  ASSERT(remote.task(), !(msghdr.msg_flags & MSG_CTRUNC))
      << "Control message was truncated; error in receiving fd in "
         "AutoRemoteSyscalls::child_recvmsg(). msghdr.msg_flags: "
      << HEX(msghdr.msg_flags) << "\n"
      << "This error has been most likely caused by a process\n"
      << "exceeding the max allowed open files limit set by\n"
      << "Linux. Please consult `man 1 ulimit' and `man 1 prlimit' to\n"
      << "learn how the max open files limit may be changed/checked.\n"
      << "As usual, always carefully think through all implications of\n"
      << "changing the process limits on your programs before making any\n"
      << "changes.\n\n"
      << "If the above Assertion still fails, then (a) The limit you set was\n"
      << "not high enough, or (b) the program could be opening files in an\n"
      << "unbounded fashion, or (c) there is some other reason why socket\n"
      << "control messages are being truncated and file descriptors cannot be\n"
      << "received via SCM_RIGHTS.";

  int their_fd = remote.task()->read_mem(msg.remote_cmsgdata(), &ok);
  if (!ok) {
    ASSERT(remote.task(), errno == ESRCH || errno == EIO);
    LOG(debug) << "Failed to read msg";
    return -ESRCH;
  }
  return their_fd;
}

#define MAX_FDS_READ 2

// Try to read a single-character message from `sock`. Will collect
// up to MAX_FDS_READ fds in an SCM_RIGHTS control message and return those
// fds. Returns an empty vector if reading the message fails.
static vector<ScopedFd> maybe_receive_fds(ScopedFd& sock, bool blocking = true) {
  vector<ScopedFd> ret;
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  char ch;
  struct iovec iov = { &ch, 1 };
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  char cmsgbuf[(CMSG_SPACE(MAX_FDS_READ * sizeof(int)))];
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  int flags = MSG_CMSG_CLOEXEC;
  if (!blocking) {
    flags |= MSG_DONTWAIT;
  }
  if (recvmsg(sock, &msg, flags) < 0) {
    return ret;
  }

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SCM_RIGHTS) {
    FATAL() << "Invalid cmsg";
  }
  int num_fds = (cmsg->cmsg_len - CMSG_LEN(0))/sizeof(int);
  for (int i = 0; i < num_fds; i++) {
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg) + i*sizeof(int), sizeof(int));
    DEBUG_ASSERT(fd >= 0);
    ret.push_back(ScopedFd(fd));
  }
  return ret;
}

static void sendmsg_socket(ScopedFd& sock, int fd_to_send)
{
  fd_message<NativeArch> msg;

  struct msghdr *msgp = (struct msghdr*)&msg.msg;
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(msgp);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(fd_to_send));
  *(int*)CMSG_DATA(cmsg) = fd_to_send;

  if (0 > sendmsg(sock, msgp, 0)) {
    FATAL() << "Failed to send fd";
  }
}

static Task* thread_group_leader_for_fds(Task* t) {
  for (Task* tt : t->fd_table()->task_set()) {
    if (tt->tgid() == tt->rec_tid && !tt->seen_ptrace_exit_event()) {
      return tt;
    }
  }
  return nullptr;
}

template <typename Arch> ScopedFd AutoRemoteSyscalls::retrieve_fd_arch(int fd) {
  ScopedFd ret;
  if (!pid_fd.is_open()) {
    // Try to use pidfd_getfd to get the fd without round-tripping to the tracee.
    // pidfd_getfd requires a threadgroup leader, so find one if we can.
    Task* tg_leader_for_fds = thread_group_leader_for_fds(t);
    if (tg_leader_for_fds) {
      // N.B.: pidfd_open fds are always cloexec
      pid_fd = ScopedFd(::syscall(NativeArch::pidfd_open, tg_leader_for_fds->tid, 0));
      ASSERT(t, pid_fd.is_open() || errno == ENOSYS)
        << "Error in pidfd_open errno=" << errno_name(errno);
    }
  }
  if (pid_fd.is_open()) {
    ret = ScopedFd(::syscall(NativeArch::pidfd_getfd, pid_fd.get(), fd, 0));
    if (ret.is_open()) {
      return ret;
    }
    if (errno == EBADF) {
      // This can happen when the child was unexpectedly killed.
      return ret;
    }
    ASSERT(t, errno == ENOSYS) << "Failed in pidfd_getfd errno=" << errno_name(errno);
  }

  // Clear out any pending message in the socket.
  maybe_receive_fds(task()->session().tracee_socket_receiver_fd(), false);

  long child_syscall_result =
      child_sendmsg<Arch>(*this, task()->session().tracee_fd_number(), fd);
  if (child_syscall_result == -ESRCH) {
    return ret;
  }
  ASSERT(t, child_syscall_result > 0) << "Failed to sendmsg() in tracee; err="
                                      << errno_name(-child_syscall_result);
  vector<ScopedFd> fds = maybe_receive_fds(task()->session().tracee_socket_fd());
  ASSERT(t, !fds.empty()) << "Failed to receive fd";
  ASSERT(t, fds.size() == 1);
  return std::move(fds[0]);
}

ScopedFd AutoRemoteSyscalls::retrieve_fd(int fd) {
  RR_ARCH_FUNCTION(retrieve_fd_arch, arch(), fd);
}

template <typename Arch> int AutoRemoteSyscalls::send_fd_arch(const ScopedFd &our_fd) {
  if (!our_fd.is_open()) {
    return -EBADF;
  }

  // Clear out any pending message from the socket.
  maybe_receive_fds(task()->session().tracee_socket_receiver_fd(), false);

  LOG(debug) << "Sending fd " << our_fd.get() << " via socket fd " << task()->session().tracee_socket_fd().get();
  sendmsg_socket(task()->session().tracee_socket_fd(), our_fd.get());

  long child_syscall_result =
      child_recvmsg<Arch>(*this, task()->session().tracee_fd_number());
  // If the child died before reading the message from the socket,
  // the message will still be in the socket buffer and will be received
  // the next time we try to send something to a tracee. That's why
  // before using tracee_socket_receiver_fd we need to drain up to one message
  // from it.
  ASSERT(t, child_syscall_result >= 0 || child_syscall_result == -ESRCH)
    << "Failed to recvmsg() in tracee; err=" << errno_name(-child_syscall_result);
  return child_syscall_result;
}

int AutoRemoteSyscalls::send_fd(const ScopedFd &our_fd) {
  RR_ARCH_FUNCTION(send_fd_arch, arch(), our_fd);
}

void AutoRemoteSyscalls::infallible_close_syscall_if_alive(int child_fd) {
  infallible_syscall_if_alive(syscall_number_for_close(arch()), child_fd);
}

int AutoRemoteSyscalls::infallible_send_fd_if_alive(const ScopedFd &our_fd) {
  int child_fd = send_fd(our_fd);
  ASSERT(t, child_fd >= 0 || (child_fd == -ESRCH && !t->session().is_replaying()))
    << "Failed to send fd; err=" << errno_name(-child_fd);
  return child_fd;
}

void AutoRemoteSyscalls::infallible_send_fd_dup(const ScopedFd& our_fd, int dup_to, int dup3_flags) {
  int remote_fd = infallible_send_fd_if_alive(our_fd);
  ASSERT(t, remote_fd >= 0);
  if (remote_fd != dup_to) {
    long ret = infallible_syscall(syscall_number_for_dup3(arch()), remote_fd,
                                  dup_to, dup3_flags);
    ASSERT(task(), ret == dup_to);
    infallible_close_syscall_if_alive(remote_fd);
  }
}

remote_ptr<void> AutoRemoteSyscalls::infallible_mmap_syscall_if_alive(
    remote_ptr<void> addr, size_t length, int prot, int flags, int child_fd,
    uint64_t offset_bytes) {
  ASSERT(t, offset_bytes % page_size() == 0)
    << "mmap offset (" << offset_bytes << ") must be multiple of page size ("
    << page_size() << ")";
  // The first syscall argument is called "arg 1", so
  // our syscall-arg-index template parameter starts
  // with "1".
  remote_ptr<void> ret =
      has_mmap2_syscall(arch())
          ? infallible_syscall_ptr_if_alive(syscall_number_for_mmap2(arch()), addr,
                                            length, prot, flags, child_fd,
                                            (off_t)offset_bytes / 4096)
          : infallible_syscall_ptr_if_alive(syscall_number_for_mmap(arch()), addr,
                                            length, prot, flags, child_fd,
                                            offset_bytes);
  if (flags & MAP_FIXED) {
    if (ret) {
      ASSERT(t, addr == ret) << "MAP_FIXED at " << addr << " but got " << ret;
    } else {
      if (!t->vm()->has_mapping(addr)) {
        KernelMapping km = t->vm()->read_kernel_mapping(t, addr);
        if (km.size()) {
          ASSERT(t, km.start() == addr && km.size() == ceil_page_size(length));
          // The mapping was created. Pretend this call succeeded.
          ret = addr;
        }
      }
    }
  }
  return ret;
}

bool AutoRemoteSyscalls::infallible_munmap_syscall_if_alive(
    remote_ptr<void> addr, size_t length) {
  long ret = infallible_syscall_if_alive(syscall_number_for_munmap(arch()),
                                         addr, length);
  if (ret) {
    if (t->vm()->has_mapping(addr)) {
      KernelMapping km = t->vm()->read_kernel_mapping(t, addr);
      if (!km.size()) {
        // The unmap happened but the task must have died before
        // reporting the status.
        ret = 0;
      }
    }
  }
  return !ret;
}

int64_t AutoRemoteSyscalls::infallible_lseek_syscall(int fd, int64_t offset,
                                                     int whence) {
  switch (arch()) {
    case x86: {
      AutoRestoreMem mem(*this, &offset, sizeof(int64_t));
      infallible_syscall(syscall_number_for__llseek(arch()), fd, offset >> 32,
                         offset, mem.get(), whence);
      return t->read_mem(mem.get().cast<int64_t>());
    }
    case x86_64:
    case aarch64:
      return infallible_syscall(syscall_number_for_lseek(arch()), fd, offset,
                                whence);
    default:
      ASSERT(t, false) << "Unknown arch";
      return -1;
  }
}

bool AutoRemoteSyscalls::check_syscall_result(long ret, int syscallno, bool allow_death) {
  if (word_size(t->arch()) == 4) {
    // Sign-extend ret because it can be a 32-bit negative errno
    ret = (int)ret;
  }
  if (ret == -ESRCH && allow_death && !t->session().is_replaying()) {
    return true;
  }
  if (-4096 < ret && ret < 0) {
    string extra_msg;
    if (is_open_syscall(syscallno, arch())) {
      extra_msg = " opening " + t->read_c_str(t->regs().arg1());
    } else if (is_openat_syscall(syscallno, arch()) || is_openat2_syscall(syscallno, arch())) {
      extra_msg = " opening " + t->read_c_str(t->regs().arg2());
    } else if (is_mremap_syscall(syscallno, arch())) {
      AddressSpace::print_process_maps(t);
    } else if (is_mmap_syscall(syscallno, arch())) {
      AddressSpace::print_process_maps(t);
      if ((t->regs().arg4() & MAP_ANONYMOUS) == 0) {
        extra_msg = " mmapping " + t->file_name_of_fd(t->regs().arg5());
      }
    }
    ASSERT(t, false) << "Syscall " << syscall_name(syscallno, arch())
                     << " failed with errno " << errno_name(-ret) << extra_msg
                     << " arg1=0x" << hex << t->regs().arg1() << " arg2=0x" << t->regs().arg2()
                     << " arg3=0x" << hex << t->regs().arg3() << " arg4=0x" << t->regs().arg4()
                     << " arg5=0x" << hex << t->regs().arg5() << " arg6=0x" << t->regs().arg6();
  }
  return false;
}

void AutoRemoteSyscalls::finish_direct_mmap(
                               remote_ptr<void> rec_addr, size_t length,
                               int prot, int flags,
                               const string& backing_file_name,
                               int backing_file_open_flags,
                               off64_t backing_offset_bytes,
                               struct stat& real_file, string& real_file_name) {
  int fd;

  LOG(debug) << "directly mmap'ing " << length << " bytes of "
             << backing_file_name << " at offset "
             << HEX(backing_offset_bytes);

  ASSERT(task(), !(flags & MAP_GROWSDOWN));

  /* Open in the tracee the file that was mapped during
   * recording. */
  {
    AutoRestoreMem child_str(*this, backing_file_name.c_str());
    if (word_size(t->arch()) == 4) {
      backing_file_open_flags |= RR_LARGEFILE_32;
    }
    fd = infallible_syscall(syscall_number_for_openat(arch()), -1,
                            child_str.get().as_int(),
                            backing_file_open_flags);
  }
  /* And mmap that file. */
  infallible_mmap_syscall_if_alive(rec_addr, length,
                          /* (We let SHARED|WRITEABLE
                          * mappings go through while
                          * they're not handled properly,
                          * but we shouldn't do that.) */
                          prot, (flags & ~MAP_SYNC) | MAP_FIXED, fd,
                          /* MAP_SYNC is used to request direct mapping
                          * (DAX) from the filesystem for persistent
                          * memory devices (requires
                          * MAP_SHARED_VALIDATE). Drop it for the
                          * backing file. */
                          backing_offset_bytes);

  // While it's open, grab the link reference.
  real_file = task()->stat_fd(fd);
  real_file_name = task()->file_name_of_fd(fd);

  /* Don't leak the tmp fd.  The mmap doesn't need the fd to
   * stay open. */
  infallible_close_syscall_if_alive(fd);
}


} // namespace rr
