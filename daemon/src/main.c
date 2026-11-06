/*
 * main.c: arkivectord entry point. Placeholder until phase D2 (startup sequencing,
 * config, sync, HTTP). Prints the version.
 */
#include <stdio.h>
#include <string.h>

#include "version.h"

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", av_version_string());
        return 0;
    }
    fprintf(stderr, "%s: not functional yet (phase D0 skeleton)\n", av_version_string());
    return 0;
}
