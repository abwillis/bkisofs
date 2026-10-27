/*
 * isoedit.c - command-line ISO editor using bkisofs
 *
 * Adds files/directories, creates directories, and recursively removes
 * files/directories from an existing ISO image, writing a new image.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "bk.h"

/* Host path separators (for SOURCE arguments only; ISO paths always use '/') */
#if defined(__OS2__) || defined(WINDOWS_BUILD)
#define IS_HOST_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define IS_HOST_SEP(c) ((c) == '/')
#endif

typedef enum OperationType
{
    OP_ADD,
    OP_MKDIR,
    OP_REMOVE
} OperationType;

typedef struct Operation
{
    OperationType type;
    const char *arg1;
    const char *arg2;
} Operation;

static const char *programName = "isoedit";
static int quietMode = 0;
static int skipErrors = 0;
static unsigned long skippedCount = 0;
static int progressLineOpen = 0;
static time_t lastDotTime = 0;

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: %s [options] INPUT.ISO OUTPUT.ISO OPERATION...\n"
        "\n"
        "Operations are applied from left to right:\n"
        "  -a, --add SOURCE ISO-DIR     Add a file or directory (recursively)\n"
        "                               into ISO-DIR; missing parents are created\n"
        "  -m, --mkdir ISO-DIR          Create a directory, including parents\n"
        "  -r, --remove ISO-PATH        Remove a file or directory recursively\n"
        "\n"
        "Options (may appear anywhere):\n"
        "  -R, --replace                Replace an existing item when adding\n"
        "  -f, --follow-symlinks        Follow filesystem symbolic links\n"
        "  -s, --skip-errors            Skip unreadable/unsupported items inside\n"
        "                               added directories instead of aborting\n"
        "  -B, --allow-unbootable       Allow edits that remove the El Torito\n"
        "                               boot file\n"
        "  -n, --no-dedup               Do not scan for duplicate file contents\n"
        "                               (faster on large images)\n"
        "  -q, --quiet                  Suppress progress messages\n"
        "  -h, --help                   Show this help text\n"
        "  --                           End of options\n"
        "\n"
        "ISO paths are absolute, use '/', and are case-sensitive.  '.' and '..'\n"
        "components are not allowed.  The output is written to a temporary file\n"
        "and renamed into place only after a successful write.\n"
        "\n"
        "Examples:\n"
        "  %s old.iso new.iso --add README.md /docs\n"
        "  %s old.iso new.iso --mkdir /boot/tools --add tools /boot\n"
        "  %s old.iso new.iso --remove /obsolete --remove /old.txt\n",
        programName, programName, programName, programName);
}

static void finishProgressLine(void)
{
    if(progressLineOpen)
    {
        fputc('\n', stderr);
        progressLineOpen = 0;
    }
}

static void reportBkError(const char *action, int rc)
{
    finishProgressLine();
    fprintf(stderr, "%s: %s: %s (%d)\n",
            programName, action, bk_get_error_string(rc), rc);
}

static void printDot(void)
{
    time_t now;

    if(quietMode)
        return;
    now = time(NULL);
    if(now != lastDotTime)
    {
        fputc('.', stderr);
        fflush(stderr);
        progressLineOpen = 1;
        lastDotTime = now;
    }
}

static void readProgress(VolInfo *volInfo)
{
    (void)volInfo;
    printDot();
}

static void addProgress(VolInfo *volInfo)
{
    (void)volInfo;
    printDot();
}

static void writeProgress(VolInfo *volInfo, double percentComplete)
{
    static int lastPercent = -1;
    int percent = (int)percentComplete;
    (void)volInfo;

    if(!quietMode && percent != lastPercent)
    {
        fprintf(stderr, "\rWriting ISO: %3d%%", percent);
        fflush(stderr);
        progressLineOpen = 1;
        lastPercent = percent;
    }
}

static bool skipWarning(const char *message)
{
    finishProgressLine();
    fprintf(stderr, "%s: warning: %s (skipped)\n", programName, message);
    skippedCount++;
    return true;
}

/* Collapse duplicate slashes, convert '\' to '/', strip trailing slashes,
 * force a leading '/', and optionally append a trailing '/'. */
static char *normalizeIsoPath(const char *path, int directoryPath)
{
    size_t inputLen;
    size_t i;
    size_t j;
    char *out;

    if(path == NULL || path[0] == '\0')
        return NULL;

    inputLen = strlen(path);
    out = malloc(inputLen + 3);
    if(out == NULL)
        return NULL;

    j = 0;
    if(path[0] != '/' && path[0] != '\\')
        out[j++] = '/';

    for(i = 0; i < inputLen; i++)
    {
        char ch = path[i] == '\\' ? '/' : path[i];
        if(ch == '/' && j > 0 && out[j - 1] == '/')
            continue;
        out[j++] = ch;
    }

    while(j > 1 && out[j - 1] == '/')
        j--;

    if(directoryPath && (j == 0 || out[j - 1] != '/'))
        out[j++] = '/';

    if(j == 0)
        out[j++] = '/';

    out[j] = '\0';
    return out;
}

static int isoPathHasDotComponent(const char *path)
{
    const char *p = path;

    while(*p != '\0')
    {
        const char *start;
        size_t len;

        while(*p == '/')
            p++;
        start = p;
        while(*p != '\0' && *p != '/')
            p++;
        len = (size_t)(p - start);
        if((len == 1 && start[0] == '.') ||
           (len == 2 && start[0] == '.' && start[1] == '.'))
            return 1;
    }
    return 0;
}

static int normalizeChecked(const char *path, int directoryPath, char **out)
{
    *out = normalizeIsoPath(path, directoryPath);
    if(*out == NULL)
        return BKERROR_OUT_OF_MEMORY;
    if(isoPathHasDotComponent(*out))
    {
        free(*out);
        *out = NULL;
        return BKERROR_NAME_INVALID;
    }
    return 1;
}

static BkFileBase *findChild(BkDir *dir, const char *name)
{
    BkFileBase *child = dir->children;
    while(child != NULL)
    {
        if(strcmp(child->name, name) == 0)
            return child;
        child = child->next;
    }
    return NULL;
}

/* Returns the node for a normalized ISO path, or NULL if not present. */
static BkFileBase *findItem(VolInfo *volInfo, const char *path)
{
    char *copy;
    char *part;
    char *saveptr = NULL;
    BkDir *dir = &volInfo->dirTree;
    BkFileBase *item = BK_BASE_PTR(&volInfo->dirTree);

    copy = malloc(strlen(path) + 1);
    if(copy == NULL)
        return NULL;
    strcpy(copy, path);

    part = strtok_r(copy, "/", &saveptr);
    while(part != NULL)
    {
        item = findChild(dir, part);
        if(item == NULL)
            break;
        part = strtok_r(NULL, "/", &saveptr);
        if(part != NULL)
        {
            if(!IS_DIR(item->posixFileMode))
            {
                item = NULL;
                break;
            }
            dir = BK_DIR_PTR(item);
        }
    }

    free(copy);
    return item;
}

static int createDirectoryPath(VolInfo *volInfo, const char *path)
{
    char *copy;
    char *part;
    char *saveptr = NULL;
    char *parent;
    int rc = 1;

    copy = normalizeIsoPath(path, 1);
    if(copy == NULL)
        return BKERROR_OUT_OF_MEMORY;
    if(strcmp(copy, "/") == 0)
    {
        free(copy);
        return 1;
    }

    parent = malloc(strlen(copy) + 1);
    if(parent == NULL)
    {
        free(copy);
        return BKERROR_OUT_OF_MEMORY;
    }
    strcpy(parent, "/");

    part = strtok_r(copy, "/", &saveptr);
    while(part != NULL)
    {
        BkDir *existingDir = NULL;
        char *current;
        size_t currentLen = strlen(parent) + strlen(part) + 2;

        current = malloc(currentLen);
        if(current == NULL)
        {
            rc = BKERROR_OUT_OF_MEMORY;
            break;
        }
        snprintf(current, currentLen, "%s%s/", parent, part);

        rc = bk_get_dir_from_string(volInfo, current, &existingDir);
        if(rc <= 0)
        {
            rc = bk_create_dir(volInfo, parent, part);
            if(rc <= 0)
            {
                free(current);
                break;
            }

            rc = bk_get_dir_from_string(volInfo, current, &existingDir);
            if(rc <= 0)
            {
                free(current);
                break;
            }

            /* bk_create_dir() does not zero the new node.  mangleDir() uses
             * original9660name when its first byte is non-zero, so garbage
             * there produces corrupt (possibly unterminated) 9660 names. */
            memset(BK_BASE_PTR(existingDir)->original9660name, 0,
                   sizeof(BK_BASE_PTR(existingDir)->original9660name));
        }

        strcpy(parent, current);
        free(current);
        part = strtok_r(NULL, "/", &saveptr);
    }

    free(parent);
    free(copy);
    return rc;
}

/* Strip trailing separators from SOURCE and extract the name to use on the
 * ISO.  bkisofs' getLastNameFromPath() returns an empty name for a source
 * with a trailing slash ("tools/"), which would create a nameless entry. */
static int prepareSource(const char *source, char **trimmedOut, char **nameOut)
{
    size_t len = strlen(source);
    size_t nameStart;
    size_t nameLen;
    char *trimmed;
    char *name;

    while(len > 1 && IS_HOST_SEP(source[len - 1]))
        len--;

    trimmed = malloc(len + 1);
    if(trimmed == NULL)
        return BKERROR_OUT_OF_MEMORY;
    memcpy(trimmed, source, len);
    trimmed[len] = '\0';

    nameStart = len;
    while(nameStart > 0 && !IS_HOST_SEP(trimmed[nameStart - 1]))
        nameStart--;
    nameLen = len - nameStart;

    if(nameLen == 0 ||
       strcmp(trimmed + nameStart, ".") == 0 ||
       strcmp(trimmed + nameStart, "..") == 0)
    {
        free(trimmed);
        return BKERROR_NAME_INVALID;
    }
    if(nameLen > NCHARS_FILE_ID_MAX_STORE - 1)
    {
        free(trimmed);
        return BKERROR_MAX_NAME_LENGTH_EXCEEDED;
    }

    name = malloc(nameLen + 1);
    if(name == NULL)
    {
        free(trimmed);
        return BKERROR_OUT_OF_MEMORY;
    }
    memcpy(name, trimmed + nameStart, nameLen + 1);

    *trimmedOut = trimmed;
    *nameOut = name;
    return 1;
}

static int loadImage(VolInfo *volInfo, const char *path, bool scanDuplicates,
                     bool followSymlinks)
{
    int pass;
    int rc = 0;

    for(pass = 0; pass < 2; pass++)
    {
        bool keepPosix = (pass == 0);

        rc = bk_init_vol_info(volInfo, scanDuplicates);
        if(rc <= 0)
        {
            reportBkError("initializing volume", rc);
            return rc;
        }
        bk_set_follow_symlinks(volInfo, followSymlinks);
        if(skipErrors)
            volInfo->warningCbk = skipWarning;

        rc = bk_open_image(volInfo, path);
        if(rc <= 0)
        {
            reportBkError("opening input image", rc);
            bk_destroy_vol_info(volInfo);
            return rc;
        }

        rc = bk_read_vol_info(volInfo);
        if(rc <= 0)
        {
            reportBkError("reading volume information", rc);
            bk_destroy_vol_info(volInfo);
            return rc;
        }

        if(volInfo->filenameTypes & FNTYPE_ROCKRIDGE)
            rc = bk_read_dir_tree(volInfo, FNTYPE_ROCKRIDGE, keepPosix, readProgress);
        else if(volInfo->filenameTypes & FNTYPE_JOLIET)
            rc = bk_read_dir_tree(volInfo, FNTYPE_JOLIET, false, readProgress);
        else
            rc = bk_read_dir_tree(volInfo, FNTYPE_9660, false, readProgress);
        finishProgressLine();

        if(rc > 0)
            return rc;

        if(rc == BKERROR_NO_POSIX_PRESENT && keepPosix &&
           (volInfo->filenameTypes & FNTYPE_ROCKRIDGE))
        {
            fprintf(stderr,
                    "%s: warning: some Rock Ridge records lack permissions; "
                    "re-reading with default permissions\n", programName);
            bk_destroy_vol_info(volInfo);
            continue;
        }

        reportBkError("reading directory tree", rc);
        bk_destroy_vol_info(volInfo);
        return rc;
    }

    return rc;
}

static int writeOutput(VolInfo *volInfo, const char *inputPath,
                       const char *outputPath, int filenameTypes)
{
    struct stat inStat;
    struct stat outStat;
    int outputExisted = 0;
    char *tempPath;
    size_t tempLen;
    mode_t mode;
    mode_t mask;
    int fd;
    int rc;

    if(stat(outputPath, &outStat) == 0)
    {
        outputExisted = 1;
        if(S_ISDIR(outStat.st_mode))
        {
            fprintf(stderr, "%s: output path is a directory: %s\n",
                    programName, outputPath);
            return 0;
        }
        if(stat(inputPath, &inStat) == 0 &&
           inStat.st_dev == outStat.st_dev && inStat.st_ino == outStat.st_ino)
        {
            fprintf(stderr, "%s: output must not be the input image: %s\n",
                    programName, outputPath);
            return 0;
        }
    }

    tempLen = strlen(outputPath) + 8;
    tempPath = malloc(tempLen);
    if(tempPath == NULL)
    {
        reportBkError("allocating temporary path", BKERROR_OUT_OF_MEMORY);
        return 0;
    }
    snprintf(tempPath, tempLen, "%s.XXXXXX", outputPath);

    fd = mkstemp(tempPath);
    if(fd == -1)
    {
        fprintf(stderr, "%s: cannot create temporary file %s: %s\n",
                programName, tempPath, strerror(errno));
        free(tempPath);
        return 0;
    }
    close(fd);

    rc = bk_write_image(tempPath, volInfo, time(NULL), filenameTypes, writeProgress);
    finishProgressLine();
    if(rc <= 0)
    {
        reportBkError("writing output image", rc);
        unlink(tempPath);
        free(tempPath);
        return 0;
    }

    /* bk_write_image() creates the file 0600; keep the old output's mode,
     * otherwise apply the normal umask. */
    if(outputExisted)
        mode = outStat.st_mode & 07777;
    else
    {
        mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }
    if(chmod(tempPath, mode) != 0)
        fprintf(stderr, "%s: warning: cannot set permissions on %s: %s\n",
                programName, tempPath, strerror(errno));

    if(rename(tempPath, outputPath) != 0)
    {
        fprintf(stderr, "%s: cannot rename %s to %s: %s\n",
                programName, tempPath, outputPath, strerror(errno));
        unlink(tempPath);
        free(tempPath);
        return 0;
    }

    free(tempPath);
    return 1;
}

static int bootWasLost(int hadBoot, const VolInfo *volInfo)
{
    return hadBoot && volInfo->bootMediaType == BOOT_MEDIA_NONE;
}

int main(int argc, char **argv)
{
    VolInfo volInfo;
    Operation *operations = NULL;
    size_t operationCount = 0;
    size_t operationCapacity = 0;
    const char *inputPath = NULL;
    const char *outputPath = NULL;
    int replaceExisting = 0;
    int followSymlinks = 0;
    int allowUnbootable = 0;
    int scanDuplicates = 1;
    int endOfOptions = 0;
    int filenameTypes;
    int rc;
    int argi;
    size_t opIndex;
    int loaded = 0;

    if(argc > 0 && argv[0] != NULL)
        programName = argv[0];

    for(argi = 1; argi < argc; argi++)
    {
        const char *arg = argv[argi];
        Operation op;

        memset(&op, 0, sizeof(op));

        if(endOfOptions || arg[0] != '-' || arg[1] == '\0')
        {
            if(inputPath == NULL)
                inputPath = arg;
            else if(outputPath == NULL)
                outputPath = arg;
            else
            {
                fprintf(stderr, "%s: unexpected argument: %s\n", programName, arg);
                free(operations);
                return EXIT_FAILURE;
            }
            continue;
        }

        if(strcmp(arg, "--") == 0)
        {
            endOfOptions = 1;
            continue;
        }
        if(strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            usage(stdout);
            free(operations);
            return EXIT_SUCCESS;
        }
        if(strcmp(arg, "-R") == 0 || strcmp(arg, "--replace") == 0)
        {
            replaceExisting = 1;
            continue;
        }
        if(strcmp(arg, "-f") == 0 || strcmp(arg, "--follow-symlinks") == 0)
        {
            followSymlinks = 1;
            continue;
        }
        if(strcmp(arg, "-s") == 0 || strcmp(arg, "--skip-errors") == 0)
        {
            skipErrors = 1;
            continue;
        }
        if(strcmp(arg, "-B") == 0 || strcmp(arg, "--allow-unbootable") == 0)
        {
            allowUnbootable = 1;
            continue;
        }
        if(strcmp(arg, "-n") == 0 || strcmp(arg, "--no-dedup") == 0)
        {
            scanDuplicates = 0;
            continue;
        }
        if(strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0)
        {
            quietMode = 1;
            continue;
        }

        if(strcmp(arg, "-a") == 0 || strcmp(arg, "--add") == 0)
        {
            if(argi + 2 >= argc || argv[argi + 1][0] == '\0' || argv[argi + 2][0] == '\0')
            {
                fprintf(stderr, "%s: --add requires non-empty SOURCE and ISO-DIR\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            op.type = OP_ADD;
            op.arg1 = argv[argi + 1];
            op.arg2 = argv[argi + 2];
            argi += 2;
        }
        else if(strcmp(arg, "-m") == 0 || strcmp(arg, "--mkdir") == 0)
        {
            if(argi + 1 >= argc || argv[argi + 1][0] == '\0')
            {
                fprintf(stderr, "%s: --mkdir requires a non-empty ISO-DIR\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            op.type = OP_MKDIR;
            op.arg1 = argv[argi + 1];
            argi += 1;
        }
        else if(strcmp(arg, "-r") == 0 || strcmp(arg, "--remove") == 0)
        {
            if(argi + 1 >= argc || argv[argi + 1][0] == '\0')
            {
                fprintf(stderr, "%s: --remove requires a non-empty ISO-PATH\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            op.type = OP_REMOVE;
            op.arg1 = argv[argi + 1];
            argi += 1;
        }
        else
        {
            fprintf(stderr, "%s: unknown option: %s\n", programName, arg);
            free(operations);
            return EXIT_FAILURE;
        }

        if(operationCount == operationCapacity)
        {
            size_t newCapacity = operationCapacity == 0 ? 8 : operationCapacity * 2;
            Operation *newOperations = realloc(operations, newCapacity * sizeof(*operations));
            if(newOperations == NULL)
            {
                fprintf(stderr, "%s: out of memory\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            operations = newOperations;
            operationCapacity = newCapacity;
        }
        operations[operationCount++] = op;
    }

    if(inputPath == NULL || outputPath == NULL)
    {
        usage(stderr);
        free(operations);
        return EXIT_FAILURE;
    }
    if(operationCount == 0)
    {
        fprintf(stderr, "%s: at least one operation is required\n", programName);
        free(operations);
        return EXIT_FAILURE;
    }

    rc = loadImage(&volInfo, inputPath, scanDuplicates != 0, followSymlinks != 0);
    if(rc <= 0)
    {
        free(operations);
        return EXIT_FAILURE;
    }
    loaded = 1;

    for(opIndex = 0; opIndex < operationCount; opIndex++)
    {
        Operation *op = &operations[opIndex];

        if(op->type == OP_MKDIR)
        {
            char *dirPath = NULL;

            rc = normalizeChecked(op->arg1, 1, &dirPath);
            if(rc <= 0)
            {
                reportBkError(op->arg1, rc);
                goto failure;
            }
            rc = createDirectoryPath(&volInfo, dirPath);
            if(rc <= 0)
            {
                reportBkError(dirPath, rc);
                free(dirPath);
                goto failure;
            }
            if(!quietMode)
                fprintf(stderr, "Created directory %s\n", dirPath);
            free(dirPath);
        }
        else if(op->type == OP_REMOVE)
        {
            char *itemPath = NULL;
            int hadBoot;

            rc = normalizeChecked(op->arg1, 0, &itemPath);
            if(rc <= 0)
            {
                reportBkError(op->arg1, rc);
                goto failure;
            }
            if(strcmp(itemPath, "/") == 0)
            {
                fprintf(stderr, "%s: refusing to remove the ISO root directory\n", programName);
                free(itemPath);
                goto failure;
            }
            if(findItem(&volInfo, itemPath) == NULL)
            {
                fprintf(stderr, "%s: item not found on image: %s\n", programName, itemPath);
                free(itemPath);
                goto failure;
            }

            hadBoot = volInfo.bootMediaType != BOOT_MEDIA_NONE;
            rc = bk_delete(&volInfo, itemPath);
            if(rc <= 0)
            {
                reportBkError(itemPath, rc);
                free(itemPath);
                goto failure;
            }
            if(bootWasLost(hadBoot, &volInfo))
            {
                if(!allowUnbootable)
                {
                    fprintf(stderr,
                            "%s: removing %s removes the El Torito boot file; "
                            "the output would not boot (use --allow-unbootable)\n",
                            programName, itemPath);
                    free(itemPath);
                    goto failure;
                }
                fprintf(stderr, "%s: warning: output image will not be bootable\n", programName);
            }
            if(!quietMode)
                fprintf(stderr, "Removed %s\n", itemPath);
            free(itemPath);
        }
        else
        {
            char *sourcePath = NULL;
            char *sourceName = NULL;
            char *destDir = NULL;
            char *destItem = NULL;
            BkFileBase *existing;
            int repointBoot = 0;

            rc = prepareSource(op->arg1, &sourcePath, &sourceName);
            if(rc <= 0)
            {
                finishProgressLine();
                fprintf(stderr, "%s: cannot use '%s' as SOURCE: %s "
                        "(name the file or directory explicitly, e.g. ../tools)\n",
                        programName, op->arg1, bk_get_error_string(rc));
                goto failure;
            }

            rc = normalizeChecked(op->arg2, 1, &destDir);
            if(rc <= 0)
            {
                reportBkError(op->arg2, rc);
                free(sourcePath);
                free(sourceName);
                goto failure;
            }

            rc = createDirectoryPath(&volInfo, destDir);
            if(rc <= 0)
            {
                reportBkError(destDir, rc);
                free(sourcePath);
                free(sourceName);
                free(destDir);
                goto failure;
            }

            destItem = malloc(strlen(destDir) + strlen(sourceName) + 1);
            if(destItem == NULL)
            {
                reportBkError("building destination path", BKERROR_OUT_OF_MEMORY);
                free(sourcePath);
                free(sourceName);
                free(destDir);
                goto failure;
            }
            strcpy(destItem, destDir);
            strcat(destItem, sourceName);

            existing = findItem(&volInfo, destItem);
            if(existing != NULL)
            {
                int oldWasRegular = IS_REG_FILE(existing->posixFileMode);
                int hadBoot = volInfo.bootMediaType != BOOT_MEDIA_NONE;

                if(!replaceExisting)
                {
                    fprintf(stderr,
                            "%s: destination already exists: %s (use --replace)\n",
                            programName, destItem);
                    free(sourcePath);
                    free(sourceName);
                    free(destDir);
                    free(destItem);
                    goto failure;
                }

                rc = bk_delete(&volInfo, destItem);
                if(rc <= 0)
                {
                    reportBkError(destItem, rc);
                    free(sourcePath);
                    free(sourceName);
                    free(destDir);
                    free(destItem);
                    goto failure;
                }

                if(bootWasLost(hadBoot, &volInfo))
                {
                    if(oldWasRegular)
                        repointBoot = 1;
                    else if(!allowUnbootable)
                    {
                        fprintf(stderr,
                                "%s: replacing %s removes the El Torito boot file; "
                                "the output would not boot (use --allow-unbootable)\n",
                                programName, destItem);
                        free(sourcePath);
                        free(sourceName);
                        free(destDir);
                        free(destItem);
                        goto failure;
                    }
                    else
                        fprintf(stderr, "%s: warning: output image will not be bootable\n",
                                programName);
                }
            }

            rc = bk_add_as(&volInfo, sourcePath, destDir, sourceName, addProgress);
            finishProgressLine();
            if(rc <= 0)
            {
                reportBkError(sourcePath, rc);
                free(sourcePath);
                free(sourceName);
                free(destDir);
                free(destItem);
                goto failure;
            }

            if(repointBoot)
            {
                BkFileBase *added = findItem(&volInfo, destItem);

                if(added != NULL && IS_REG_FILE(added->posixFileMode))
                {
                    rc = bk_set_boot_file(&volInfo, destItem);
                    if(rc <= 0)
                    {
                        reportBkError(destItem, rc);
                        free(sourcePath);
                        free(sourceName);
                        free(destDir);
                        free(destItem);
                        goto failure;
                    }
                    fprintf(stderr,
                            "%s: warning: boot file %s replaced; El Torito now uses the "
                            "new file (no emulation)\n", programName, destItem);
                }
                else if(!allowUnbootable)
                {
                    fprintf(stderr,
                            "%s: boot file %s was replaced by a non-regular file; "
                            "the output would not boot (use --allow-unbootable)\n",
                            programName, destItem);
                    free(sourcePath);
                    free(sourceName);
                    free(destDir);
                    free(destItem);
                    goto failure;
                }
            }

            if(!quietMode)
                fprintf(stderr, "Added %s as %s\n", sourcePath, destItem);
            free(sourcePath);
            free(sourceName);
            free(destDir);
            free(destItem);
        }
    }

    /* Always write all three name types, matching the original tools.
     * Writing only the source image's types would shorten new long names to
     * 8.3 on plain ISO9660 images and break symbolic links. */
    filenameTypes = FNTYPE_9660 | FNTYPE_ROCKRIDGE | FNTYPE_JOLIET;

    if(!writeOutput(&volInfo, inputPath, outputPath, filenameTypes))
        goto failure;

    bk_destroy_vol_info(&volInfo);
    free(operations);
    if(skippedCount > 0)
        fprintf(stderr, "%s: warning: %lu item(s) skipped\n", programName, skippedCount);
    if(!quietMode)
        fprintf(stderr, "Wrote %s\n", outputPath);
    return EXIT_SUCCESS;

failure:
    if(loaded)
        bk_destroy_vol_info(&volInfo);
    free(operations);
    return EXIT_FAILURE;
}
