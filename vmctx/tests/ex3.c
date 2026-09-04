/*
 * ex3 — exec, with no fork in between. Used to make a context exec twice:
 *
 *   ex1 ex3 /bin/echo hi     -> ex1 forks, the child execs ex3, ex3 execs echo
 *
 * Built both ways. A static ex3 makes the second exec the first one that
 * involves a dynamic loader at all, which is how "the second exec is broken"
 * gets separated from "something the first program's loader left behind".
 */
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: ex3 <program> [args...]\n");
		return 2;
	}
	execv(argv[1], argv + 1);
	printf("ex3: execv failed\n");
	return 127;
}
