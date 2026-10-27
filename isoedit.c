/*
 * isoedit.c - command-line ISO editor using bkisofs
 *
 * Supports adding files/directories, creating directories, and recursively
 * removing files/directories from an existing ISO image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bk.h"

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

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: %s [options] INPUT.ISO OUTPUT.ISO OPERATION...\n"
        "\n"
        "Operations are applied from left to right:\n"
        "  -a, --add SOURCE ISO-DIR     Add a file or directory recursively\n"
        "  -m, --mkdir ISO-DIR          Create a directory, including parents\n"
        "  -r, --remove ISO-PATH        Remove a file or directory recursively\n"
        "\n"
        "Options:\n"
        "  -R, --replace                Replace an existing item when adding\n"
        "  -f, --follow-symlinks        Follow filesystem symbolic links\n"
        "  -q, --quiet                  Suppress progress messages\n"
        "  -h, --help                   Show this help text\n"
        "\n"
        "ISO paths are absolute and use '/'.  Directory paths may be written\n"
        "with or without a trailing slash.  INPUT.ISO and OUTPUT.ISO must be\n"
        "different files because bkisofs does not overwrite the source image.\n"
        "\n"
        "Examples:\n"
        "  %s old.iso new.iso --add README.md /docs\n"
        "  %s old.iso new.iso --mkdir /boot/tools --add tools /boot\n"
        "  %s old.iso new.iso --remove /obsolete --remove /old.txt\n",
        programName, programName, programName, programName);
}

static void reportBkError(const char *action, int rc)
{
    fprintf(stderr, "%s: %s: %s (%d)\n",
            programName, action, bk_get_error_string(rc), rc);
}

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

static const char *lastPathComponent(const char *path)
{
    const char *end = path + strlen(path);
    const char *start;

    while(end > path && (end[-1] == '/' || end[-1] == '\\'))
        end--;
    start = end;
    while(start > path && start[-1] != '/' && start[-1] != '\\')
        start--;
    return start;
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

static int itemExists(VolInfo *volInfo, const char *path)
{
    char *copy;
    char *part;
    char *saveptr = NULL;
    BkDir *dir = &volInfo->dirTree;
    BkFileBase *item = NULL;

    copy = normalizeIsoPath(path, 0);
    if(copy == NULL)
        return 0;
    if(strcmp(copy, "/") == 0)
    {
        free(copy);
        return 1;
    }

    part = strtok_r(copy, "/", &saveptr);
    while(part != NULL)
    {
        item = findChild(dir, part);
        if(item == NULL)
        {
            free(copy);
            return 0;
        }
        part = strtok_r(NULL, "/", &saveptr);
        if(part != NULL)
        {
            if(!IS_DIR(item->posixFileMode))
            {
                free(copy);
                return 0;
            }
            dir = BK_DIR_PTR(item);
        }
    }

    free(copy);
    return 1;
}

static int createDirectoryPath(VolInfo *volInfo, const char *path)
{
    char *copy;
    char *part;
    char *saveptr = NULL;
    char *parent;
    size_t pathLen;
    int rc = 1;

    copy = normalizeIsoPath(path, 1);
    if(copy == NULL)
        return BKERROR_OUT_OF_MEMORY;
    if(strcmp(copy, "/") == 0)
    {
        free(copy);
        return 1;
    }

    pathLen = strlen(copy);
    parent = malloc(pathLen + 1);
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
        }

        strcpy(parent, current);
        free(current);
        part = strtok_r(NULL, "/", &saveptr);
    }

    free(parent);
    free(copy);
    return rc;
}

static int buildDestinationItemPath(const char *source, const char *destDir,
                                    char **result)
{
    const char *name = lastPathComponent(source);
    char *normalizedDir = normalizeIsoPath(destDir, 1);
    size_t nameLen = strlen(name);
    size_t dirLen;

    if(normalizedDir == NULL)
        return BKERROR_OUT_OF_MEMORY;
    while(nameLen > 0 && (name[nameLen - 1] == '/' || name[nameLen - 1] == '\\'))
        nameLen--;
    if(nameLen == 0)
    {
        free(normalizedDir);
        return BKERROR_MISFORMED_PATH;
    }

    dirLen = strlen(normalizedDir);
    *result = malloc(dirLen + nameLen + 1);
    if(*result == NULL)
    {
        free(normalizedDir);
        return BKERROR_OUT_OF_MEMORY;
    }
    memcpy(*result, normalizedDir, dirLen);
    memcpy(*result + dirLen, name, nameLen);
    (*result)[dirLen + nameLen] = '\0';
    free(normalizedDir);
    return 1;
}

static void readProgress(VolInfo *volInfo)
{
    (void)volInfo;
    if(!quietMode)
    {
        fputc('.', stderr);
        fflush(stderr);
    }
}

static void addProgress(VolInfo *volInfo)
{
    (void)volInfo;
    if(!quietMode)
    {
        fputc('.', stderr);
        fflush(stderr);
    }
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
        lastPercent = percent;
        if(percent >= 100)
            fputc('\n', stderr);
    }
}

int main(int argc, char **argv)
{
    VolInfo volInfo;
    Operation *operations = NULL;
    size_t operationCount = 0;
    size_t operationCapacity = 0;
    const char *inputPath;
    const char *outputPath;
    int replaceExisting = 0;
    int followSymlinks = 0;
    int filenameTypes;
    int rc;
    int argi;
    size_t opIndex;
    int initialized = 0;

    if(argc > 0 && argv[0] != NULL)
        programName = argv[0];

    if(argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    if(argc < 4)
    {
        usage(stderr);
        return EXIT_FAILURE;
    }

    inputPath = argv[1];
    outputPath = argv[2];
    argi = 3;

    while(argi < argc)
    {
        Operation op;
        memset(&op, 0, sizeof(op));

        if(strcmp(argv[argi], "-R") == 0 || strcmp(argv[argi], "--replace") == 0)
        {
            replaceExisting = 1;
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "-f") == 0 || strcmp(argv[argi], "--follow-symlinks") == 0)
        {
            followSymlinks = 1;
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "-q") == 0 || strcmp(argv[argi], "--quiet") == 0)
        {
            quietMode = 1;
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0)
        {
            usage(stdout);
            free(operations);
            return EXIT_SUCCESS;
        }
        if(strcmp(argv[argi], "-a") == 0 || strcmp(argv[argi], "--add") == 0)
        {
            if(argi + 2 >= argc)
            {
                fprintf(stderr, "%s: --add requires SOURCE and ISO-DIR\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            op.type = OP_ADD;
            op.arg1 = argv[argi + 1];
            op.arg2 = argv[argi + 2];
            argi += 3;
        }
        else if(strcmp(argv[argi], "-m") == 0 || strcmp(argv[argi], "--mkdir") == 0)
        {
            if(argi + 1 >= argc)
            {
                fprintf(stderr, "%s: --mkdir requires ISO-DIR\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            op.type = OP_MKDIR;
            op.arg1 = argv[argi + 1];
            argi += 2;
        }
        else if(strcmp(argv[argi], "-r") == 0 || strcmp(argv[argi], "--remove") == 0)
        {
            if(argi + 1 >= argc)
            {
                fprintf(stderr, "%s: --remove requires ISO-PATH\n", programName);
                free(operations);
                return EXIT_FAILURE;
            }
            op.type = OP_REMOVE;
            op.arg1 = argv[argi + 1];
            argi += 2;
        }
        else
        {
            fprintf(stderr, "%s: unknown option or operation: %s\n", programName, argv[argi]);
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

    if(operationCount == 0)
    {
        fprintf(stderr, "%s: at least one operation is required\n", programName);
        free(operations);
        return EXIT_FAILURE;
    }

    rc = bk_init_vol_info(&volInfo, true);
    if(rc <= 0)
    {
        reportBkError("initializing volume", rc);
        free(operations);
        return EXIT_FAILURE;
    }
    initialized = 1;
    bk_set_follow_symlinks(&volInfo, followSymlinks != 0);

    rc = bk_open_image(&volInfo, inputPath);
    if(rc <= 0)
    {
        reportBkError("opening input image", rc);
        goto failure;
    }
    rc = bk_read_vol_info(&volInfo);
    if(rc <= 0)
    {
        reportBkError("reading volume information", rc);
        goto failure;
    }

    if(volInfo.filenameTypes & FNTYPE_ROCKRIDGE)
        rc = bk_read_dir_tree(&volInfo, FNTYPE_ROCKRIDGE, true, readProgress);
    else if(volInfo.filenameTypes & FNTYPE_JOLIET)
        rc = bk_read_dir_tree(&volInfo, FNTYPE_JOLIET, false, readProgress);
    else
        rc = bk_read_dir_tree(&volInfo, FNTYPE_9660, false, readProgress);
    if(!quietMode)
        fputc('\n', stderr);
    if(rc <= 0)
    {
        reportBkError("reading directory tree", rc);
        goto failure;
    }

    for(opIndex = 0; opIndex < operationCount; opIndex++)
    {
        Operation *op = &operations[opIndex];

        if(op->type == OP_MKDIR)
        {
            rc = createDirectoryPath(&volInfo, op->arg1);
            if(rc <= 0)
            {
                reportBkError(op->arg1, rc);
                goto failure;
            }
            if(!quietMode)
                fprintf(stderr, "Created directory %s\n", op->arg1);
        }
        else if(op->type == OP_REMOVE)
        {
            char *normalized = normalizeIsoPath(op->arg1, 0);
            if(normalized == NULL)
            {
                rc = BKERROR_OUT_OF_MEMORY;
                reportBkError("normalizing removal path", rc);
                goto failure;
            }
            if(strcmp(normalized, "/") == 0)
            {
                fprintf(stderr, "%s: refusing to remove the ISO root directory\n", programName);
                free(normalized);
                goto failure;
            }
            if(!itemExists(&volInfo, normalized))
            {
                fprintf(stderr, "%s: item not found on image: %s\n", programName, normalized);
                free(normalized);
                goto failure;
            }
            rc = bk_delete(&volInfo, normalized);
            if(rc <= 0)
            {
                reportBkError(normalized, rc);
                free(normalized);
                goto failure;
            }
            if(!quietMode)
                fprintf(stderr, "Removed %s\n", normalized);
            free(normalized);
        }
        else
        {
            char *destDir = normalizeIsoPath(op->arg2, 1);
            char *destItem = NULL;
            if(destDir == NULL)
            {
                rc = BKERROR_OUT_OF_MEMORY;
                reportBkError("normalizing add destination", rc);
                goto failure;
            }

            rc = createDirectoryPath(&volInfo, destDir);
            if(rc <= 0)
            {
                reportBkError(destDir, rc);
                free(destDir);
                goto failure;
            }
            rc = buildDestinationItemPath(op->arg1, destDir, &destItem);
            if(rc <= 0)
            {
                reportBkError("building destination path", rc);
                free(destDir);
                goto failure;
            }

            if(itemExists(&volInfo, destItem))
            {
                if(!replaceExisting)
                {
                    fprintf(stderr,
                            "%s: destination already exists: %s; use --replace to replace it\n",
                            programName, destItem);
                    free(destItem);
                    free(destDir);
                    goto failure;
                }
                rc = bk_delete(&volInfo, destItem);
                if(rc <= 0)
                {
                    reportBkError(destItem, rc);
                    free(destItem);
                    free(destDir);
                    goto failure;
                }
            }

            rc = bk_add(&volInfo, op->arg1, destDir, addProgress);
            if(!quietMode)
                fputc('\n', stderr);
            if(rc <= 0)
            {
                reportBkError(op->arg1, rc);
                free(destItem);
                free(destDir);
                goto failure;
            }
            if(!quietMode)
                fprintf(stderr, "Added %s as %s\n", op->arg1, destItem);
            free(destItem);
            free(destDir);
        }
    }

    filenameTypes = volInfo.filenameTypes &
                    (FNTYPE_9660 | FNTYPE_ROCKRIDGE | FNTYPE_JOLIET);
    if((filenameTypes & FNTYPE_9660) == 0)
        filenameTypes |= FNTYPE_9660;

    rc = bk_write_image(outputPath, &volInfo, time(NULL), filenameTypes, writeProgress);
    if(rc <= 0)
    {
        reportBkError("writing output image", rc);
        goto failure;
    }

    bk_destroy_vol_info(&volInfo);
    free(operations);
    if(!quietMode)
        fprintf(stderr, "Wrote %s\n", outputPath);
    return EXIT_SUCCESS;

failure:
    if(initialized)
        bk_destroy_vol_info(&volInfo);
    free(operations);
    return EXIT_FAILURE;
}
