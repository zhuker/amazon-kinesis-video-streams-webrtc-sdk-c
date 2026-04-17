#include "SamplesCommon.h"

STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;

    if (pSize == NULL) {
        printf("[Samples] readFrameFromDisk(): NULL size arg\n");
        goto CleanUp;
    }

    size = *pSize;

    retStatus = readFile(frameFilePath, TRUE, pFrame, &size);
    if (retStatus != STATUS_SUCCESS) {
        printf("[Samples] readFile(%s): 0x%08x\n", frameFilePath, retStatus);
        goto CleanUp;
    }

CleanUp:
    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }
    return retStatus;
}
