Linux VM context



This project adds virtual machine context to Linux.



Motivation:

Currently Linux only supports processes. Switching to VM context (VM enter and VM exit) is typically handled by KVM and is not natively supported. This project introduces VM context to be natively supported by Linux. A VM context is mostly the same as a process, except for the fact that Linux specially handles process context switch for this context by doing VM enter and VM exit at the point of context switch. By supporting native VM context, everything that may operate on a process may also operate on this context.



Objective:

1. The context may be used to support a full VM, or a single application (meaning, that the context may only run on user privilege).
2. The context should have the same capability to handle page faults or CPU faults: either redirect to other processes (mostly used if the context is hosting a VM), or self handle (needed if the context is hosting a user level program).
3. If the handling is redirected, need proper tools (system calls) to be able to handle the fault (for example, read/write the VM blob, and all other tools).
4. Ability for migration and remote execution to occur (explore what additional tools are needed for this to occur. Maybe we already support this with all the previous requirements completed).

5\. We can spawn this context remotely: for example, I can spawn a remote chrome, where the user process will run under this VM context, remotely on another machine. The remote machine will encapsulate the context to use its CPU and memory, but all system calls should be forwarded to the local host for processing. The end result should be a process that executes user level code and user level memory on remote machine, but with the support of this context, shows the result locally.



Steps of implementation:

1. Implement Intel and AMD VM context. Add proper tools to enable spawning this context and debugging this context (for example, bash command 'startvm', 'dumpvm' etc.). Spawning this context on some process to verify the correctness (for example, 'startvm ls' should do the 'ls' function correctly).
2. Implement the ability to remotely execute the context.



Tools and continuation:

Use the current folder to verify the result. You already have the source code for Linux, and you already have PXE boot setup.



Stop and prompt if you need me to reboot the other machine to PXE to verify some result.

Make sure that at all steps, you have sufficient debugging tool for me to verify (for example, the ability to dump the context completely for debugging).



