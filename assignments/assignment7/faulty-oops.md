# Faulty Driver Kernel Oops Analysis

## Command Used
```sh
echo "hello_world" > /dev/faulty
````

## Kernel Oops Output

```text
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: hello(O) faulty(O) scull(O)
pc : faulty_write+0x10/0x20 [faulty]
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
```

## Explanation

The crash is caused by a NULL pointer dereference in the `faulty` module’s
write function (`faulty_write`). The oops shows:

* The faulting address is `0x0`, meaning a NULL pointer was accessed.
* The program counter (`pc`) indicates the crash occurred inside
  `faulty_write`.
* The call trace confirms the crash happened during a `write()` system call.

Using the function name and offset (`faulty_write+0x10`), the exact
faulting line can be located in the faulty driver source.

