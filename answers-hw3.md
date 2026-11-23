# HW3 answers

Fill in this file and hand it in with your code (your HW1 and HW2
answers stay in answers.md and answers-hw2.md). Replace each
`(your answer)` with two or three sentences. Keep the question numbers
(part.question), and do not delete the questions.

- **Name:**
- **Machine and environment you tested on** (QEMU with or without KVM, or Bochs):
- **How far did you get?** (for example "the guest boots to a shell", or "stuck in part 4"):


## Part 1: A guest in ring 3

### Question 1.1

> In 64-bit mode the guest kernel cannot run in ring 1 (see "Why not ring 1?"). Suppose it did anyway, with the same shadow page tables. Name one piece of host memory it could then read or write that it cannot from ring 3, and the SDM rule that allows it.

**Answer:** (your answer)

### Question 1.2

> The guest kernel is linked at 4 TB. What would go wrong if it kept the host's `KERNBASE`, 8 TB, and the hypervisor mapped it there in the shadows? Think of the first trap from the guest.

**Answer:** (your answer)

### Question 1.3

> `pv_enter()` points `TSS.RSP0` just below its own frame. Suppose it left `RSP0` at the top of the kernel stack. Describe what the guest's first exit would overwrite and what would happen when `pv_exit` returns.

**Answer:** (your answer)

### Question 1.4

> A host timer tick arrives while the guest runs. List, in order, the functions it passes through in the host kernel until the guest runs again, and say why `pvhandle()` calls `yield()` for it.

**Answer:** (your answer)

### Question 1.5

> The hypercall gate has DPL 3, so the guest's user programs can reach it as well as the guest's kernel. Why can't the hypervisor simply give the gate DPL 1 or 2 and let only the guest kernel through? (Which CPL does the guest kernel run at?)

**Answer:** (your answer)

## Part 2: Shadow page tables

### Question 2.1

> `pvfill()` gives every shadow entry `PTE_U`, including the guest kernel's pages. What keeps the guest's user programs from reading the guest kernel's memory? What would they be able to do if `pvfill()` skipped the mode check in step 3 of the contract? Give an example with xv6's `struct proc`.

**Answer:** (your answer)

### Question 2.2

> In the stats line at the beginning of this handout, the guest made 144 `CR3` loads and took 10907 shadow fills, about 75 per load. Most of these refill the guest kernel's own mappings, which are the same in every one of the guest's page tables. Propose a way to keep them across a `CR3` load. What must the guest promise for it to be correct, and which feature of the real CPU does the same (SDM 5.10.2.4)?

**Answer:** (your answer)

### Question 2.3

> Suppose `pvwalk()` did not check the table addresses in the guest's entries. Describe what a malicious guest kernel could write in its page tables to make the host read memory outside the guest, and what the host would do with what it read.

**Answer:** (your answer)

### Question 2.4

> A guest user program reads the byte at 0x40000100000, the guest kernel's first page. Follow the access step by step, from the CPU's fault to the guest kernel's message that kills the program: which shadow is loaded, why it has no entry, what `pvwalk()` and `pvfill()` return, and what `pvhandle()` does next. (Part 4 delivers the fault; you can answer from `pvhandle()`'s code.)

**Answer:** (your answer)

## Part 3: Paravirtualizing the guest kernel

### Question 3.1

> xv6 reads `IF` with `readrflags()` in `mycpu()`, `pushcli()`, `popcli()` and `sched()`. For each, say what would go wrong in the guest with the native version, as if the others had been fixed. Why does the same code work in the host kernel?

**Answer:** (your answer)

### Question 3.2

> `cli()` makes no hypercall. How and when does the hypervisor learn that the guest's interrupts are off? Suppose it delivered the timer interrupt anyway while the guest holds `ptable.lock` in `scheduler()`. What would the guest's timer handler do, and how would the guest fail?

**Answer:** (your answer)

### Question 3.3

> Suppose `sti()` did not check `pvcpu.pending`. Which interrupts would wait, and for how long at most? Think of a disk request in Part 5: the device finishes it and raises IRQ 14 while the guest is inside `iderw()`, holding `disk.lock`. Which programs in the guest would get slower, and by how much per request?

**Answer:** (your answer)

### Question 3.4

> `MOV` from `CR2` is privileged, so `rcr2()` must change anyway. But suppose it were not: would the guest's page-fault handler read the right address from the real `CR2`? Think of what happens between the guest's fault and the moment its handler reads `CR2`, right after a `CR3` load emptied the shadows.

**Answer:** (your answer)

## Part 4: Traps and interrupts

### Question 4.1

> `pviret()` keeps only the `FL_GUEST` bits of the guest's `rflags` and forces `FL_IF` on. Name two bits outside `FL_GUEST` that a guest could set if `pviret()` copied `rflags` unmasked, and what each would let it do to the host. Why must `IF` be forced on, rather than copied?

**Answer:** (your answer)

### Question 4.2

> A program in the guest executes `int $0x82` with `rax` = 5 and `rdi` = the guest-physical address of a page table it built in its own memory. What does the hypervisor do, and what does the guest kernel print? Why is the error code 1042, and what would happen if `pvhcall()` served the hypercall instead?

**Answer:** (your answer)

### Question 4.3

> `pvtrap()` builds the frame on `pv.kstack` for a trap from the guest's user mode, but on the current stack for a trap from its kernel mode. Describe what would go wrong if it always used `pv.kstack`, with a timer interrupt that arrives while the guest kernel runs a system call.

**Answer:** (your answer)

### Question 4.4

> The hypervisor delivers interrupts only when it enters the guest. You type a key while the guest runs a program that loops in user mode without system calls. Which exit lets the guest see the key, and what is the longest it can take? Why is there no such delay for the disk interrupt that `runpv` raises in Part 5?

**Answer:** (your answer)

## Part 5: A virtio disk

### Question 5.1

> Count the exits for one 512-byte block in HW2 (xv6's IDE driver, `idestart()` and `ideintr()`, against `runxv6`) and in HW3. Then use the stats line above: what fraction of all exits went to `runpv`, and what was most of the rest?

**Answer:** (your answer)

### Question 5.2

> `dorequest()` copies each descriptor out of guest memory once. Suppose a device ran on another CPU while the guest kept running, and it read `d[1].len` from guest memory once for the range check and once more for the copy. Describe how a malicious guest kernel would use that to make the device write outside the disk image. Why is our `runpv` safe from it even if it read twice?

**Answer:** (your answer)

### Question 5.3

> A bug in `dorequest()` (say, no range check) lets the guest write anywhere in `runpv`'s memory. What is the worst the guest can do to the host then, and which check in the kernel stops it there? Compare with a device model that runs inside the host kernel, as KVM's `vhost` drivers do: what would the same bug allow, and why do hypervisors accept that risk?

**Answer:** (your answer)

### Question 5.4

> `iderw()` writes the available ring entry, then increments `avail.idx`, with barriers. Our device runs only while the guest is stopped. Would the driver still work without the barriers, or with the two stores swapped? Describe a device for which it would not, and what it would do wrong.

**Answer:** (your answer)

### Question 5.5

> When `viowrite(VIRTIO_MMIO_QUEUE_NOTIFY, 0)` returns, our device has already finished the request. Could `iderw()` read the used ring right then, and skip the interrupt and its two exits? What would the driver then assume about the device, and which real devices would break it?

**Answer:** (your answer)
