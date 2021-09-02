/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#define TAOS_RANDOM_FILE_FAIL_TEST
#include "os.h"
#include "taoserror.h"
#include "taosmsg.h"
#include "tchecksum.h"
#include "tfile.h"
#include "twal.h"
#include "walInt.h"

static int32_t walRestoreWalFile(SWal *pWal, void *pVnode, FWalWrite writeFp, char *name, int64_t fileId);

//TODO: call back vnode to reset offset
int32_t walRenew(void *handle) {
  if (handle == NULL) return 0;

  SWal *  pWal = handle;
  int32_t code = 0;

  if (pWal->stop) {
    wDebug("vgId:%d, do not create a new wal file", pWal->vgId);
    return 0;
  }

  pthread_mutex_lock(&pWal->mutex);

  if (tfValid(pWal->tfd)) {
    tfClose(pWal->tfd);
    wDebug("vgId:%d, file:%s, it is closed while renew", pWal->vgId, pWal->name);
  }

#if 0
  if (pWal->keep == TAOS_WAL_KEEP) {
    pWal->fileId = 0;
  } else {
#endif
    if (walGetNewFile(pWal, &pWal->writeFileId) != 0)
      pWal->writeFileId = 0;
    if(pWal->writeFileId < 0) pWal->writeFileId = 0;
    pWal->writeFileId++;
    pWal->offset = 0;
#if 0
  }
#endif

  snprintf(pWal->name, sizeof(pWal->name), "%s/%s%" PRId32, pWal->path, WAL_PREFIX, pWal->writeFileId);
  pWal->tfd = tfOpenM(pWal->name, O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);

  if (!tfValid(pWal->tfd)) {
    code = TAOS_SYSTEM_ERROR(errno);
    wError("vgId:%d, file:%s, failed to open since %s", pWal->vgId, pWal->name, strerror(errno));
  } else {
    wDebug("vgId:%d, file:%s, it is created and open while renew", pWal->vgId, pWal->name);
  }

  pthread_mutex_unlock(&pWal->mutex);

  return code;
}

void walRemoveOneOldFile(void *handle) {
  SWal *pWal = handle;
  if (pWal == NULL) return;
  if (!tfValid(pWal->tfd)) return;

  pthread_mutex_lock(&pWal->mutex);
  pWal->restoreFileId++;

  // remove the oldest wal file
  int32_t oldFileId = -1;
  while (walGetOldFile(pWal, pWal->restoreFileId, WAL_FILE_NUM, &oldFileId) == 0) {
    char walName[WAL_FILE_LEN] = {0};
    snprintf(walName, sizeof(walName), "%s/%s%" PRId32, pWal->path, WAL_PREFIX, oldFileId);

    if (remove(walName) < 0) {
      wError("vgId:%d, file:%s, failed to remove since %s", pWal->vgId, walName, strerror(errno));
    } else {
      wInfo("vgId:%d, file:%s, it is removed", pWal->vgId, walName);
    }
  }

  pthread_mutex_unlock(&pWal->mutex);
}

void walRemoveAllOldFiles(void *handle) {
  if (handle == NULL) return;

  SWal *  pWal = handle;
  int32_t fileId = -1;

  pthread_mutex_lock(&pWal->mutex);
  
  tfClose(pWal->tfd);
  wDebug("vgId:%d, file:%s, it is closed before remove all wals", pWal->vgId, pWal->name);

  while (walGetNextFile(pWal, &fileId) >= 0) {
    snprintf(pWal->name, sizeof(pWal->name), "%s/%s%" PRId32, pWal->path, WAL_PREFIX, fileId);

    if (remove(pWal->name) < 0) {
      wError("vgId:%d, wal:%p file:%s, failed to remove since %s", pWal->vgId, pWal, pWal->name, strerror(errno));
    } else {
      wInfo("vgId:%d, wal:%p file:%s, it is removed", pWal->vgId, pWal, pWal->name);
    }
  }
  pWal->startFileId = -1;
  pWal->restoreFileId = -1;
  pWal->writeFileId = -1;
  pthread_mutex_unlock(&pWal->mutex);
}

int32_t walLifeCycleCheck(void *handle, int8_t action) {
  SWal *pWal = handle;
  if (pWal == NULL) return 0;
  if (action == TAOS_WAL_CHECK_RENEW) {
    //start commit
    if (/*no reader */ 1) return walRenew(handle);
  } else if(action == TAOS_WAL_CHECK_PRUNE) {
    //end commit
    if (/* no reader */ 1) walRemoveOneOldFile(handle);
  }
  return 0;
}

#if defined(WAL_CHECKSUM_WHOLE)

static void walUpdateChecksum(SWalHead *pHead) {
  pHead->sver = 2;
  pHead->cksum = 0;
  pHead->cksum = taosCalcChecksum(0, (uint8_t *)pHead, sizeof(*pHead) + pHead->len);
}

static int walValidateChecksum(SWalHead *pHead) {
  if (pHead->sver == 0) { // for compatible with wal before sver 1
    return taosCheckChecksumWhole((uint8_t *)pHead, sizeof(*pHead));
  } else if (pHead->sver >= 1) {
    uint32_t cksum = pHead->cksum;
    pHead->cksum = 0;
    return taosCheckChecksum((uint8_t *)pHead, sizeof(*pHead) + pHead->len, cksum);
  }

  return 0;
}

#endif

int32_t walSetRestore(void *handle, uint64_t fOffset, int32_t restoreFileId) {
  if (handle == NULL) return -1;
  SWal *  pWal = handle;
  pWal->fOffset = fOffset;
  pWal->restoreFileId = restoreFileId;
  return 0;
}

// return num of bytes if succeed
int32_t walWrite(void *handle, SWalHead *pHead) {
  if (handle == NULL) return -1;

  SWal *  pWal = handle;
  int32_t code = 0;

  // no wal
  if (!tfValid(pWal->tfd)) return 0;
  if (pWal->level == TAOS_WAL_NOLOG) return 0;
  if (pHead->version <= pWal->version) return 0;

  pHead->signature = WAL_SIGNATURE;
#if defined(WAL_CHECKSUM_WHOLE)
  walUpdateChecksum(pHead);
#else
  pHead->sver = 0;
  taosCalcChecksumAppend(0, (uint8_t *)pHead, sizeof(SWalHead));
#endif

  int32_t contLen = pHead->len + sizeof(SWalHead);

  pthread_mutex_lock(&pWal->mutex);

  if ((code = (int32_t)tfWrite(pWal->tfd, pHead, contLen)) != contLen) {
    //TODO: consider truncating here
    if(code > 0) pWal->offset += code;
    code = TAOS_SYSTEM_ERROR(errno);
    wError("vgId:%d, file:%s, failed to write since %s", pWal->vgId, pWal->name, strerror(errno));
  } else {
    code = contLen;
    wTrace("vgId:%d, write wal, fileId:%" PRId32 " tfd:%" PRId64 " hver:%" PRId64 " wver:%" PRIu64 " len:%d", pWal->vgId,
           pWal->writeFileId, pWal->tfd, pHead->version, pWal->version, pHead->len);
    pWal->version = pHead->version;
    pWal->offset += contLen;
  }

  pthread_mutex_unlock(&pWal->mutex);

  ASSERT(contLen == pHead->len + sizeof(SWalHead));

  return code;
}

void walFsync(void *handle, bool forceFsync) {
  SWal *pWal = handle;
  if (pWal == NULL || !tfValid(pWal->tfd)) return;

  if (forceFsync || (pWal->level == TAOS_WAL_FSYNC && pWal->fsyncPeriod == 0)) {
    wTrace("vgId:%d, fileId:%" PRId32 ", do fsync", pWal->vgId, pWal->writeFileId);
    if (tfFsync(pWal->tfd) < 0) {
      wError("vgId:%d, fileId:%" PRId32 ", fsync failed since %s", pWal->vgId, pWal->writeFileId, strerror(errno));
    }
  }
}

int32_t walRestore(void *handle, void *pVnode, FWalWrite writeFp) {
  if (handle == NULL) return -1;

  SWal *  pWal = handle;
  int32_t code = 0;
  int32_t fileId = pWal->restoreFileId - 1;

  while ((code = walGetNextFile(pWal, &fileId)) >= 0) {
    /*if (fileId == pWal->fileId) continue;*/

    char walName[WAL_FILE_LEN];
    snprintf(walName, sizeof(pWal->name), "%s/%s%" PRId32, pWal->path, WAL_PREFIX, fileId);

    wInfo("vgId:%d, file:%s, will be restored", pWal->vgId, walName);
    code = walRestoreWalFile(pWal, pVnode, writeFp, walName, fileId);
    if (code != TSDB_CODE_SUCCESS) {
      wError("vgId:%d, file:%s, failed to restore since %s", pWal->vgId, walName, tstrerror(code));
      continue;
    }

    wInfo("vgId:%d, file:%s, restore success, wver:%" PRIu64, pWal->vgId, walName, pWal->version);

  }

#if 0
  if (pWal->keep != TAOS_WAL_KEEP) return TSDB_CODE_SUCCESS;
#endif

  if (fileId < 0) {
    wDebug("vgId:%d, wal file not exist, renew it", pWal->vgId);
    return walRenew(pWal);
  } else {
    // open the existing WAL file in append mode
    /*pWal->writeFileId = 0;*/
    snprintf(pWal->name, sizeof(pWal->name), "%s/%s%" PRId32, pWal->path, WAL_PREFIX, pWal->writeFileId);
    pWal->tfd = tfOpenM(pWal->name, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRWXG | S_IRWXO);
    if (!tfValid(pWal->tfd)) {
      wError("vgId:%d, file:%s, failed to open since %s", pWal->vgId, pWal->name, strerror(errno));
      return TAOS_SYSTEM_ERROR(errno);
    }
    wDebug("vgId:%d, file:%s, it is created and open while restore", pWal->vgId, pWal->name);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t walGetWalFile(void *handle, char *fileName, int32_t *fileId) {
  if (handle == NULL) return -1;
  SWal *pWal = handle;

  if (*fileId == 0) *fileId = -1;

  pthread_mutex_lock(&(pWal->mutex));

  int32_t code = walGetNextFile(pWal, fileId);
  if (code >= 0) {
    sprintf(fileName, "wal/%s%" PRId32, WAL_PREFIX, *fileId);
    code = (*fileId == pWal->writeFileId) ? 0 : 1;
  }

  wDebug("vgId:%d, get wal file, code:%d curId:%" PRId32 " outId:%" PRId32, pWal->vgId, code, pWal->writeFileId, *fileId);
  pthread_mutex_unlock(&(pWal->mutex));

  return code;
}

static void walFtruncate(SWal *pWal, int64_t tfd, int64_t offset) {
  tfFtruncate(tfd, offset);
  tfFsync(tfd);
}

static int32_t walSkipCorruptedRecord(SWal *pWal, SWalHead *pHead, int64_t tfd, int64_t *offset) {
  int64_t pos = *offset;
  while (1) {
    //TODO: replace char-by-char reading method
    pos++;

    if (tfLseek(tfd, pos, SEEK_SET) < 0) {
      wError("vgId:%d, failed to seek from corrupted wal file since %s", pWal->vgId, strerror(errno));
      return TSDB_CODE_WAL_FILE_CORRUPTED;
    }

    if (tfRead(tfd, pHead, sizeof(SWalHead)) <= 0) {
      wError("vgId:%d, read to end of corrupted wal file, offset:%" PRId64, pWal->vgId, pos);
      return TSDB_CODE_WAL_FILE_CORRUPTED;
    }

    if (pHead->signature != WAL_SIGNATURE) {
      continue;
    }

#if defined(WAL_CHECKSUM_WHOLE)
    if (pHead->sver == 0 && walValidateChecksum(pHead)) {
      wInfo("vgId:%d, wal head cksum check passed, offset:%" PRId64, pWal->vgId, pos);
      *offset = pos;
      return TSDB_CODE_SUCCESS;
    }

    if (pHead->sver >= 1) {
      if (tfRead(tfd, pHead->cont, pHead->len) < pHead->len) {
	wError("vgId:%d, read to end of corrupted wal file, offset:%" PRId64, pWal->vgId, pos);
	return TSDB_CODE_WAL_FILE_CORRUPTED;
      }

      if (walValidateChecksum(pHead)) {
	wInfo("vgId:%d, wal whole cksum check passed, offset:%" PRId64, pWal->vgId, pos);
	*offset = pos;
	return TSDB_CODE_SUCCESS;
      }
    }

#else
    if (taosCheckChecksumWhole((uint8_t *)pHead, sizeof(SWalHead))) {
      wInfo("vgId:%d, wal head cksum check passed, offset:%" PRId64, pWal->vgId, pos);
      *offset = pos;
      return TSDB_CODE_SUCCESS;
    }

#endif
  }

  return TSDB_CODE_WAL_FILE_CORRUPTED;
}
// Add SMemRowType ahead of SDataRow
static void expandSubmitBlk(SSubmitBlk *pDest, SSubmitBlk *pSrc, int32_t *lenExpand) {
  // copy the header firstly
  memcpy(pDest, pSrc, sizeof(SSubmitBlk));

  int32_t nRows = htons(pDest->numOfRows);
  int32_t dataLen = htonl(pDest->dataLen);

  if ((nRows <= 0) || (dataLen <= 0)) {
    return;
  }

  char *pDestData = pDest->data;
  char *pSrcData = pSrc->data;
  for (int32_t i = 0; i < nRows; ++i) {
    memRowSetType(pDestData, SMEM_ROW_DATA);
    memcpy(memRowDataBody(pDestData), pSrcData, dataRowLen(pSrcData));
    pDestData = POINTER_SHIFT(pDestData, memRowTLen(pDestData));
    pSrcData = POINTER_SHIFT(pSrcData, dataRowLen(pSrcData));
    ++(*lenExpand);
  }
  pDest->dataLen = htonl(dataLen + nRows * sizeof(uint8_t));
}

// Check SDataRow by comparing the SDataRow len and SSubmitBlk dataLen
static bool walIsSDataRow(void *pBlkData, int nRows, int32_t dataLen) {
  if ((nRows <= 0) || (dataLen <= 0)) {
    return true;
  }
  int32_t len = 0, kvLen = 0;
  for (int i = 0; i < nRows; ++i) {
    len += dataRowLen(pBlkData);
    if (len > dataLen) {
      return false;
    }

    /**
     * For SDataRow between version [2.1.5.0 and 2.1.6.X], it would never conflict.
     * For SKVRow between version [2.1.5.0 and 2.1.6.X], it may conflict in below scenario
     *   -  with 1st type byte 0x01 and sversion  0x0101(257), thus do further check
     */
    if (dataRowLen(pBlkData) == 257) {
      SMemRow  memRow = pBlkData;
      SKVRow   kvRow = memRowKvBody(memRow);
      int      nCols = kvRowNCols(kvRow);
      uint16_t calcTsOffset = (uint16_t)(TD_KV_ROW_HEAD_SIZE + sizeof(SColIdx) * nCols);
      uint16_t realTsOffset = (kvRowColIdx(kvRow))->offset;
      if (calcTsOffset == realTsOffset) {
        kvLen += memRowKvTLen(memRow);
      }
    }
    pBlkData = POINTER_SHIFT(pBlkData, dataRowLen(pBlkData));
  }
  if (len != dataLen) {
    return false;
  }
  if (kvLen == dataLen) {
    return false;
  }
  return true;
}
// for WAL SMemRow/SDataRow compatibility
static int walSMemRowCheck(SWalHead *pHead) {
  if ((pHead->sver < 2) && (pHead->msgType == TSDB_MSG_TYPE_SUBMIT)) {
    SSubmitMsg *pMsg = (SSubmitMsg *)pHead->cont;
    int32_t     numOfBlocks = htonl(pMsg->numOfBlocks);
    if (numOfBlocks <= 0) {
      return 0;
    }

    int32_t     nTotalRows = 0;
    SSubmitBlk *pBlk = (SSubmitBlk *)pMsg->blocks;
    for (int32_t i = 0; i < numOfBlocks; ++i) {
      int32_t dataLen = htonl(pBlk->dataLen);
      int32_t nRows = htons(pBlk->numOfRows);
      nTotalRows += nRows;
      if (!walIsSDataRow(pBlk->data, nRows, dataLen)) {
        return 0;
      }
      pBlk = (SSubmitBlk *)POINTER_SHIFT(pBlk, sizeof(SSubmitBlk) + dataLen);
    }
    ASSERT(nTotalRows >= 0);
    SWalHead *pWalHead = (SWalHead *)calloc(sizeof(SWalHead) + pHead->len + nTotalRows * sizeof(uint8_t), 1);
    if (pWalHead == NULL) {
      return -1;
    }

    memcpy(pWalHead, pHead, sizeof(SWalHead) + sizeof(SSubmitMsg));

    SSubmitMsg *pDestMsg = (SSubmitMsg *)pWalHead->cont;
    SSubmitBlk *pDestBlks = (SSubmitBlk *)pDestMsg->blocks;
    SSubmitBlk *pSrcBlks = (SSubmitBlk *)pMsg->blocks;
    int32_t     lenExpand = 0;
    for (int32_t i = 0; i < numOfBlocks; ++i) {
      expandSubmitBlk(pDestBlks, pSrcBlks, &lenExpand);
      pDestBlks = POINTER_SHIFT(pDestBlks, htonl(pDestBlks->dataLen) + sizeof(SSubmitBlk));
      pSrcBlks = POINTER_SHIFT(pSrcBlks, htonl(pSrcBlks->dataLen) + sizeof(SSubmitBlk));
    }
    if (lenExpand > 0) {
      pDestMsg->header.contLen = htonl(pDestMsg->length) + lenExpand;
      pDestMsg->length = htonl(pDestMsg->header.contLen);
      pWalHead->len = pWalHead->len + lenExpand;
    }

    memcpy(pHead, pWalHead, sizeof(SWalHead) + pWalHead->len);
    tfree(pWalHead);
  }
  return 0;
}

static int32_t walRestoreWalFile(SWal *pWal, void *pVnode, FWalWrite writeFp, char *name, int64_t fileId) {
  int32_t size = WAL_MAX_SIZE;
  void *  buffer = tmalloc(size);
  if (buffer == NULL) {
    wError("vgId:%d, file:%s, failed to open for restore since %s", pWal->vgId, name, strerror(errno));
    return TAOS_SYSTEM_ERROR(errno);
  }

  int64_t tfd = tfOpen(name, O_RDWR);
  if (!tfValid(tfd)) {
    wError("vgId:%d, file:%s, failed to open for restore since %s", pWal->vgId, name, strerror(errno));
    tfree(buffer);
    return TAOS_SYSTEM_ERROR(errno);
  } else {
    wDebug("vgId:%d, file:%s, open for restore", pWal->vgId, name);
  }

  int64_t fSize = tfSize(tfd);
  if(fSize > 0) {
    if(pWal->fOffset > fSize) {
      /*ASSERT(pWal->fOffset == fSize + 1);*/
      tfClose(tfd);
      tfree(buffer);
      return 0;
    }
  }

  if(pWal->fOffset && tfLseek(tfd, pWal->fOffset, SEEK_SET) < 0) {
    //TODO: read and search the version
    tfLseek(tfd, 0, SEEK_SET);
  }

  int32_t   code = TSDB_CODE_SUCCESS;
  int64_t   offset = 0;
  SWalHead *pHead = buffer;

  while (1) {
    int32_t ret = (int32_t)tfRead(tfd, pHead, sizeof(SWalHead));
    if (ret == 0) break;

    if (ret < 0) {
      wError("vgId:%d, file:%s, failed to read wal head since %s", pWal->vgId, name, strerror(errno));
      code = TAOS_SYSTEM_ERROR(errno);
      break;
    }

    if (ret < sizeof(SWalHead)) {
      wError("vgId:%d, file:%s, failed to read wal head, ret is %d", pWal->vgId, name, ret);
      walFtruncate(pWal, tfd, offset);
      break;
    }

#if defined(WAL_CHECKSUM_WHOLE)
    if ((pHead->sver == 0 && !walValidateChecksum(pHead)) || pHead->sver < 0 || pHead->sver > 2) {
      wError("vgId:%d, file:%s, wal head cksum is messed up, hver:%" PRIu64 " len:%d offset:%" PRId64, pWal->vgId, name,
             pHead->version, pHead->len, offset);
      code = walSkipCorruptedRecord(pWal, pHead, tfd, &offset);
      if (code != TSDB_CODE_SUCCESS) {
        walFtruncate(pWal, tfd, offset);
        break;
      }
    }

    if (pHead->len < 0 || pHead->len > size - sizeof(SWalHead)) {
      wError("vgId:%d, file:%s, wal head len out of range, hver:%" PRIu64 " len:%d offset:%" PRId64, pWal->vgId, name,
             pHead->version, pHead->len, offset);
      code = walSkipCorruptedRecord(pWal, pHead, tfd, &offset);
      if (code != TSDB_CODE_SUCCESS) {
        walFtruncate(pWal, tfd, offset);
        break;
      }
    }

    ret = (int32_t)tfRead(tfd, pHead->cont, pHead->len);
    if (ret < 0) {
      wError("vgId:%d, file:%s, failed to read wal body since %s", pWal->vgId, name, strerror(errno));
      code = TAOS_SYSTEM_ERROR(errno);
      break;
    }

    if (ret < pHead->len) {
      wError("vgId:%d, file:%s, failed to read wal body, ret:%d len:%d", pWal->vgId, name, ret, pHead->len);
      walFtruncate(pWal, tfd, offset);
      break;
    }

    if ((pHead->sver >= 1) && !walValidateChecksum(pHead)) {
      wError("vgId:%d, file:%s, wal whole cksum is messed up, hver:%" PRIu64 " len:%d offset:%" PRId64, pWal->vgId, name,
             pHead->version, pHead->len, offset);
      code = walSkipCorruptedRecord(pWal, pHead, tfd, &offset);
      if (code != TSDB_CODE_SUCCESS) {
        walFtruncate(pWal, tfd, offset);
        break;
      }
    }

#else
    if (!taosCheckChecksumWhole((uint8_t *)pHead, sizeof(SWalHead))) {
      wError("vgId:%d, file:%s, wal head cksum is messed up, hver:%" PRIu64 " len:%d offset:%" PRId64, pWal->vgId, name,
             pHead->version, pHead->len, offset);
      code = walSkipCorruptedRecord(pWal, pHead, tfd, &offset);
      if (code != TSDB_CODE_SUCCESS) {
        walFtruncate(pWal, tfd, offset);
        break;
      }
    }

    if (pHead->len < 0 || pHead->len > size - sizeof(SWalHead)) {
      wError("vgId:%d, file:%s, wal head len out of range, hver:%" PRIu64 " len:%d offset:%" PRId64, pWal->vgId, name,
             pHead->version, pHead->len, offset);
      code = walSkipCorruptedRecord(pWal, pHead, tfd, &offset);
      if (code != TSDB_CODE_SUCCESS) {
        walFtruncate(pWal, tfd, offset);
        break;
      }
    }

    ret = (int32_t)tfRead(tfd, pHead->cont, pHead->len);
    if (ret < 0) {
      wError("vgId:%d, file:%s, failed to read wal body since %s", pWal->vgId, name, strerror(errno));
      code = TAOS_SYSTEM_ERROR(errno);
      break;
    }

    if (ret < pHead->len) {
      wError("vgId:%d, file:%s, failed to read wal body, ret:%d len:%d", pWal->vgId, name, ret, pHead->len);
      walFtruncate(pWal, tfd, offset);
      break;
    }

#endif
    offset = offset + sizeof(SWalHead) + pHead->len;

    wTrace("vgId:%d, restore wal, fileId:%" PRId64 " hver:%" PRIu64 " wver:%" PRIu64 " len:%d offset:%" PRId64,
           pWal->vgId, fileId, pHead->version, pWal->version, pHead->len, offset);

    pWal->version = pHead->version;
    pWal->offset  = offset;

    // wInfo("writeFp: %ld", offset);
    if (0 != walSMemRowCheck(pHead)) {
      wError("vgId:%d, restore wal, fileId:%" PRId64 " hver:%" PRIu64 " wver:%" PRIu64 " len:%d offset:%" PRId64,
             pWal->vgId, fileId, pHead->version, pWal->version, pHead->len, offset);
      tfClose(tfd);
      tfree(buffer);
      return TAOS_SYSTEM_ERROR(errno);
    }
    (*writeFp)(pVnode, pHead, TAOS_QTYPE_WAL, NULL);
  }

  tfClose(tfd);
  tfree(buffer);

  wDebug("vgId:%d, file:%s, it is closed after restore", pWal->vgId, name);
  return code;
}

uint64_t walGetVersion(twalh param, uint64_t* offset) {
  SWal *pWal = param;
  if (pWal == 0) return 0;

  *offset = pWal->offset;
  return pWal->version;
}

// Wal version in slave (dnode1) must be reset. 
// Because after the data file is recovered from peer (dnode2), the new file version in dnode1 may become smaller than origin.
// Some new wal record cannot be written to the wal file in dnode1 for wal version not reset, then fversion and the record in wal file may inconsistent, 
// At this time, if dnode2 down, dnode1 switched to master. After dnode2 start and restore data from dnode1, data loss will occur

void walResetVersion(twalh param, uint64_t newVer, uint64_t newOff) {
  SWal *pWal = param;
  if (pWal == 0) return;
  wInfo("vgId:%d, version reset from %" PRIu64 " to %" PRIu64, pWal->vgId, pWal->version, newVer);

  pWal->version = newVer;
  pWal->offset  = newOff;
}
