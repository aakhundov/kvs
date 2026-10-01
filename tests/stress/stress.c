#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>
#include <unistd.h>

// Runs the server tests as a series of trials against a server built with
// schedule points: every trial runs each of them once, with one seed for
// every server it starts. Silent while trials pass; the first trial that
// fails stops the run and shows that trial's output.

#define TRIALS 30
#define SEED_TEXT_SIZE 32
#define LINE_SIZE 4096

#define FAILED_MARK "[  FAILED  ] "
#define FAILED_COUNT_MARK " tests, listed below:"

// every fixture whose tests start a server
static const char *const filters[] = {
    "--filter=server.*",
    "--filter=limited.*",
    "--filter=impatient.*",
    "--filter=server_startup.*",
};

// runs the tests one filter selects, with the seed in the environment and
// their output appended to (out). true when every test passed
static bool run_tests(const char *tests, const char *filter, const char *seed, FILE *out) {
  (void)fflush(out);
  pid_t kid = fork();
  if (kid < 0) {
    perror("fork");
    return false;
  }
  if (kid == 0) {
    int fd = fileno(out);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    setenv("KVS_STRESS_SEED", seed, 1);
    // the path is the Makefile's: no sanitising to do
    execl(tests, tests, filter, (char *)NULL); // NOLINT(clang-analyzer-optin.taint.GenericTaint)
    _exit(127);
  }

  int status = 0;
  while (waitpid(kid, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("waitpid");
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// prints a failed trial's output, then its seed and the tests that failed:
// utest lists each failed test once more at the end of its run, alone on
// a line after the failed mark
static void report(FILE *out, unsigned long seed) {
  (void)fflush(stdout);
  if (fseek(out, 0, SEEK_SET) != 0) {
    perror("fseek");
    return;
  }

  char failed[LINE_SIZE] = "";
  size_t failed_len = 0;
  char line[LINE_SIZE];
  while (fgets(line, sizeof line, out) != NULL) {
    (void)fputs(line, stdout);
    if (strncmp(line, FAILED_MARK, strlen(FAILED_MARK)) != 0 ||
        strstr(line, FAILED_COUNT_MARK) != NULL || strchr(line, '(') != NULL) {
      continue;
    }
    const char *name = line + strlen(FAILED_MARK);
    int n = snprintf(failed + failed_len, sizeof failed - failed_len, " %.*s",
                     (int)strcspn(name, "\n"), name);
    if (n > 0 && (size_t)n < sizeof failed - failed_len) {
      failed_len += (size_t)n;
    }
  }
  printf("---- stress: the trial with seed %lu failed:%s\n", seed,
         failed_len > 0 ? failed : " (no test named)");
}

// one trial: each filter's tests once with (seed). true when all passed
static bool run_trial(const char *tests, unsigned long seed) {
  char seed_text[SEED_TEXT_SIZE];
  (void)snprintf(seed_text, sizeof seed_text, "%lu", seed);

  FILE *out = tmpfile();
  if (out == NULL) {
    perror("tmpfile");
    return false;
  }

  bool passed = true;
  for (size_t i = 0; i < sizeof filters / sizeof filters[0] && passed; i++) {
    passed = run_tests(tests, filters[i], seed_text, out);
  }
  if (!passed) {
    report(out, seed);
  }

  (void)fclose(out);
  return passed;
}

static bool parse_seed(const char *text, unsigned long *seed) {
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || text[0] == '-') {
    return false;
  }
  *seed = value;
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    (void)fprintf(stderr, "usage: %s TESTS [SEED]\n", argv[0]);
    return 2;
  }
  const char *tests = argv[1];

  if (argc == 3) {
    unsigned long seed;
    if (!parse_seed(argv[2], &seed)) {
      (void)fprintf(stderr, "bad seed '%s'\n", argv[2]);
      return 2;
    }
    return run_trial(tests, seed) ? 0 : 1;
  }

  for (unsigned long seed = 0; seed < TRIALS; seed++) {
    if (!run_trial(tests, seed)) {
      return 1;
    }
  }
  return 0;
}
