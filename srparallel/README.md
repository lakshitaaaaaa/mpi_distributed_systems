# Two-process MPI chat

This C program uses MPICH to run a chat between exactly two MPI processes:
PE 1 (rank 0) and PE 2 (rank 1). Each terminal shows the chat history above a
fixed input line at the bottom. Sent messages appear in the sender's history
and in the other PE's history as soon as they arrive.

## Requirements

- MPICH installed, including `mpicc` and `mpiexec`
- An interactive terminal for each process
- For the local two-window example on macOS: XQuartz and `xterm`

## Build

From the directory containing `main.c`:

```sh
mpicc -Wall -Wextra -o sr2 main.c
```

Build the executable on each computer where it will run. If the computers use
different operating systems or processor architectures, build separately on
each one rather than copying the executable between them.

## Run two processes on one Mac

Open XQuartz and make sure the `DISPLAY` environment variable is set in the
Terminal session. Then run:

```sh
mpiexec -n 2 /opt/X11/bin/xterm -e ./sr2
```

This opens a separate xterm for each process. If `DISPLAY` is blank, log out
of macOS and back in after installing XQuartz, then check again with
`echo "$DISPLAY"`.

## Use the chat

- Type in the input line at the bottom of either xterm.
- Press Enter to send. The message is added to both chat histories as soon as
  it arrives.
- Use Backspace to edit the current line.
- Type `/quit` and press Enter to end the chat on both processes.

## Launch the MPI job across two computers

Install compatible MPICH versions on both computers. From the computer that
will launch the job, make sure SSH can start commands on the other computer.
For example:

```sh
ssh user@system2 hostname
```

Create a file named `hosts` in the launch directory, replacing these example
names with the computers' network hostnames:

```text
system1
system2
```

Then launch two processes, one per listed host:

```sh
mpiexec -f hosts -n 2 /path/to/sr2
```

`/path/to/sr2` must exist on both computers. MPICH Hydra documents this
host-file launch pattern in its
[process manager guide](https://github.com/pmodels/mpich/blob/main/doc/wiki/how_to/Using_the_Hydra_Process_Manager.md).

### Interactive-terminal note

Launching MPI ranks on two computers does not automatically attach each
computer's keyboard and display to its rank. A standard `mpiexec` session
usually provides input through the launching terminal, rather than a private
terminal on each host. The local two-xterm command above is for testing both
processes on one Mac. For two people to chat from separate computers, the MPI
job also needs a per-process terminal setup that attaches each rank to that
user's local terminal.
