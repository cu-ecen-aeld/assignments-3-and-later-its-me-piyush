#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);

    // Check arguments
    if (argc != 3) {
        syslog(LOG_ERR, "Invalid arguments. Usage: writer <file> <string>");
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    // Open file (create + truncate)
    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open failed: %s", strerror(errno));
        return 1;
    }

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    // Write data
    ssize_t bytes = write(fd, writestr, strlen(writestr));
    if (bytes == -1) {
        syslog(LOG_ERR, "write failed: %s", strerror(errno));
        close(fd);
        return 1;
    }

    // Close file
    if (close(fd) == -1) {
        syslog(LOG_ERR, "close failed: %s", strerror(errno));
        return 1;
    }

    closelog();
    return 0;
}
