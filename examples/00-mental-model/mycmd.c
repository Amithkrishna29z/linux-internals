/* mycmd.c
 *
 * A minimal Unix "command". It reads its arguments (argv), does something
 * with them, and returns an exit code -- exactly what /usr/bin/ls does.
 * Drop this into your PATH (Module 1) and the shell treats it like any
 * built-in tool.
 *
 * Compile:  gcc -Wall -Wextra -o mycmd mycmd.c
 * Run:      ./mycmd hello world
 *           ./mycmd            ; echo "exit code was $?"
 */

#include <stdio.h>

int main(int argc, char *argv[])
{
    /* argc = argument COUNT, including the program name.
     * argv = argument VECTOR, an array of C strings, argv[argc] == NULL.
     *
     *   ./mycmd hello world
     *   argc = 3
     *   argv[0] = "./mycmd"   <-- program name (Java's args does NOT include this)
     *   argv[1] = "hello"
     *   argv[2] = "world"
     *   argv[3] = NULL         <-- the terminator you can loop until */
    printf("I was invoked as: %s\n", argv[0]);
    printf("I received %d argument(s):\n", argc - 1);

    for (int i = 1; i < argc; i++) {
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    }

    /* Convention: 0 = success. Non-zero = a specific failure.
     * Here: fail (exit 1) if the user gave us no arguments.
     * The shell reads this as $? -- how `&&`, `||`, and scripts branch. */
    if (argc == 1) {
        fprintf(stderr, "usage: %s ARG...\n", argv[0]);  /* errors go to stderr, fd 2 */
        return 1;
    }

    return 0;
}
