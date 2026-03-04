static char *fastcopyw_id = 
	"@(#)Copyright (C) 2004-2018 H.Shirouzu and FastCopy Lab, LLC.	fastcopyw.cpp	ver3.50";
/* ========================================================================
	Project  Name			: Fast Copy file and directory (write)
	Create					: 2004-09-15(Wed)
	Update					: 2018-05-28(Mon)
	Copyright				: H.Shirouzu and FastCopy Lab, LLC.
	License					: GNU General Public License version 3
	======================================================================== */

#include "fastcopy.h"
#include <stdarg.h>
#include <stddef.h>
#include <process.h>
#include <stdio.h>
#include <time.h>
#include <random>

using namespace std;

/* ========================================================================
	NAS (Synology 等) 互換ファイル名変換
	共有フォルダ命名規則:
	 - 禁止文字: ! " # % & ' ( ) * + , / : ; < = > ? @ [ ] \ ^ ` { } | ~
	 - 制御文字 (< 0x20) および DEL (0x7F)
	 - 先頭文字に - (マイナス) またはスペース不可
	 - 末尾文字にスペース不可 (既存: 末尾 '.' / ' ' 除去)
	 - 予約名: . .. global lost+found home printers usbbackup
	 - Windows 予約名: CON PRN AUX NUL COM1-9 LPT1-9
	 - 名前長上限: 32 文字 (共有フォルダ), 一般ファイルは MAX_PATH に委ねる
	======================================================================== */

static BOOL IsNasUnsafeChar(WCHAR ch)
{
	if (ch < 0x20 || ch == 0x7f) return TRUE;

	// Synology 共有フォルダ禁止文字 完全セット
	static const WCHAR forbidden[] = L"!\"#%&'()*+,/:;<=>?@[\\]^`{|}~";
	for (const WCHAR *p = forbidden; *p; p++) {
		if (ch == *p) return TRUE;
	}
	return FALSE;
}

// NAS 予約名チェック (大文字小文字不問)
static BOOL IsNasReservedName(const WCHAR *name)
{
	// Synology 予約名
	static const WCHAR *nas_reserved[] = {
		L".", L"..", L"global", L"lost+found", L"home", L"printers", L"usbbackup", 0
	};
	for (int i = 0; nas_reserved[i]; i++) {
		if (wcsicmp(name, nas_reserved[i]) == 0) return TRUE;
	}
	// Windows 予約名 (拡張子なしステムで判定)
	static const WCHAR *win_reserved[] = {
		L"CON", L"PRN", L"AUX", L"NUL",
		L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
		L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9", 0
	};
	WCHAR stem[MAX_PATH] = {};
	const WCHAR *dot = wcschr(name, L'.');
	int stem_len = dot ? (int)(dot - name) : (int)wcslen(name);
	if (stem_len > 0 && stem_len < _countof(stem)) {
		memcpy(stem, name, stem_len * sizeof(WCHAR));
		stem[stem_len] = 0;
		for (int i = 0; win_reserved[i]; i++) {
			if (wcsicmp(stem, win_reserved[i]) == 0) return TRUE;
		}
	}
	return FALSE;
}

// NAS (ext4/btrfs) ファイル名コンポーネントの UTF-8 バイト上限
#define NAS_MAX_FILENAME_UTF8_BYTES  255

// UTF-8 エンコード時のバイト数を算出
static int CalcUtf8ByteLen(const WCHAR *str, int wchar_len)
{
	if (!str || wchar_len == 0) return 0;
	int ret = ::WideCharToMultiByte(CP_UTF8, 0, str, wchar_len, NULL, 0, NULL, NULL);
	return (ret > 0) ? ret : 0;
}

// WCHAR ファイル名を NAS の UTF-8 バイト上限に切り詰め
// 拡張子を保持し、サロゲートペアを分断しない
static int TruncateToNasUtf8Limit(WCHAR *name, int max_utf8_bytes, BOOL is_dir)
{
	int name_len = (int)wcslen(name);
	if (name_len == 0) return 0;
	if (CalcUtf8ByteLen(name, name_len) <= max_utf8_bytes) return name_len;

	// ファイルの場合、拡張子を分離して保持
	WCHAR ext_buf[64] = {};
	int ext_wlen = 0;
	int ext_bytes = 0;
	int stem_len = name_len;

	if (!is_dir) {
		const WCHAR *dot = wcsrchr(name, L'.');
		if (dot && dot != name) {
			ext_wlen = (int)(name + name_len - dot);
			if (ext_wlen < (int)_countof(ext_buf)) {
				memcpy(ext_buf, dot, ext_wlen * sizeof(WCHAR));
				ext_buf[ext_wlen] = 0;
				ext_bytes = CalcUtf8ByteLen(ext_buf, ext_wlen);
				stem_len = (int)(dot - name);
			}
		}
	}

	int avail = max_utf8_bytes - ext_bytes;
	if (avail < 3) avail = 3; // 最低 1 CJK 文字分

	// ステム部を右から縮小して UTF-8 バイト上限に収める
	while (stem_len > 0 && CalcUtf8ByteLen(name, stem_len) > avail) {
		stem_len--;
		// high surrogate (U+D800-DBFF) で切断しない
		if (stem_len > 0 && name[stem_len - 1] >= 0xD800 && name[stem_len - 1] <= 0xDBFF) {
			stem_len--;
		}
	}

	// 切り詰め後の末尾 '.' や ' ' を除去
	while (stem_len > 0 && (name[stem_len - 1] == L'.' || name[stem_len - 1] == L' ')) {
		stem_len--;
	}

	if (stem_len <= 0) {
		name[0] = L'_';
		stem_len = 1;
	}

	// 拡張子を再結合
	if (ext_wlen > 0) {
		memmove(name + stem_len, ext_buf, ext_wlen * sizeof(WCHAR));
	}
	name_len = stem_len + ext_wlen;
	name[name_len] = 0;

	return name_len;
}

static int MakeNasCompatBaseName(const WCHAR *src, WCHAR *dst, int max_dst, BOOL is_dir)
{
	int pos = 0;

	if (max_dst <= 0) return 0;

	for (; *src && pos < max_dst - 1; src++) {
		WCHAR ch = IsNasUnsafeChar(*src) ? L'_' : *src;
		dst[pos++] = ch;
	}
	dst[pos] = 0;

	// 末尾の '.' と ' ' を除去
	while (pos > 0 && (dst[pos - 1] == L'.' || dst[pos - 1] == L' ')) {
		dst[--pos] = 0;
	}

	// 先頭が '-' またはスペースの場合 '_' に置換
	if (pos > 0 && (dst[0] == L'-' || dst[0] == L' ')) {
		dst[0] = L'_';
	}

	// 全除去された場合のフォールバック
	if (pos == 0) {
		dst[pos++] = L'_';
		dst[pos] = 0;
	}

	// 予約名チェック (NAS + Windows)
	if (IsNasReservedName(dst)) {
		if (pos < max_dst - 1) {
			memmove(dst + 1, dst, (wcslen(dst) + 1) * sizeof(WCHAR));
			dst[0] = L'_';
			pos++;
		}
	}

	return	pos;
}

static int MakeNasCompatCollisionName(const WCHAR *base_name, int seq, BOOL is_dir,
									WCHAR *dst, int max_dst)
{
	WCHAR suffix[32];
	int suffix_len = swprintf(suffix, L"_%d", seq);
	const WCHAR *ext = NULL;
	int base_len = (int)wcslen(base_name);

	if (!is_dir) {
		const WCHAR *dot = wcsrchr(base_name, L'.');
		if (dot && dot != base_name) {
			ext = dot;
			base_len = (int)(dot - base_name);
		}
	}
	if (suffix_len <= 0 || max_dst <= 1) return 0;
	if (base_len + suffix_len + (ext ? (int)wcslen(ext) : 0) >= max_dst) {
		base_len = max_dst - suffix_len - (ext ? (int)wcslen(ext) : 0) - 1;
	}
	if (base_len <= 0) return 0;

	memcpy(dst, base_name, base_len * sizeof(WCHAR));
	memcpy(dst + base_len, suffix, suffix_len * sizeof(WCHAR));
	int pos = base_len + suffix_len;
	if (ext) {
		int ext_len = (int)wcslen(ext);
		memcpy(dst + pos, ext, ext_len * sizeof(WCHAR));
		pos += ext_len;
	}
	dst[pos] = 0;
	return	pos;
}

/*=========================================================================
  関  数 ： WriteThread
  概  要 ： Write 処理
  説  明 ： 
  注  意 ： 
=========================================================================*/
unsigned WINAPI FastCopy::WriteThread(void *fastCopyObj)
{
	return	((FastCopy *)fastCopyObj)->WriteThreadCore();
}

BOOL FastCopy::WriteThreadCore(void)
{
	WaitCalc	wc;

	::TlsSetValue(wcIdx, &wc);
	wc.fsType = dstFsType;

	BOOL	ret = WriteProc(dstBaseLen);

	cv.Lock();
	writeReq = NULL;
	writeReqList.Init();
	writeWaitList.Init();
	cv.Notify();
	cv.UnLock();

	return	ret;
}

BOOL FastCopy::CheckAndCreateDestDir(int dst_len)
{
	BOOL	ret = TRUE;

	dst[dst_len - 1] = 0;

	if (::GetFileAttributesW(dst) == 0xffffffff) {
		int	parent_dst_len = 0;

		parent_dst_len = dst_len - 2;
		for ( ; parent_dst_len >= 9; parent_dst_len--) { // "\\?\c:\x\..."
			if (dst[parent_dst_len -1] == '\\') break;
		}
		if (parent_dst_len < 9) parent_dst_len = 0;

		ret = parent_dst_len ? CheckAndCreateDestDir(parent_dst_len) : FALSE;

		if (!isExec || ::CreateDirectoryW(dst, NULL)) {
			if (isListingOnly || isListing) {
				PutList(dst + dstPrefixLen, PL_DIRECTORY);
			}
			curTotal->writeDirs++;
		}
		else ret = FALSE;
	}
	dst[dst_len - 1] = '\\';

	return	ret;
}

BOOL FastCopy::FinishNotify(void)
{
#ifdef TRACE_DBG
	PutList(L"\r\nTrace log", PL_ERRMSG);

	for (DWORD i=0; WCHAR *targ = trclog_get(i); i++) {
		if (*targ) {
			PutList(targ, PL_ERRMSG);
		}
	}
#endif

	endTick = ::GetTick();
	return	::PostMessage(info.hNotifyWnd, info.uNotifyMsg, END_NOTIFY, 0);
}

BOOL FastCopy::WriteProc(int dir_len)
{
	BOOL	ret = TRUE;

	while (!isAbort && (ret = RecvRequest())) {
		if (writeReq->cmd == REQ_EOF) {
			if (IsUsingDigestList() && !isAbort) {
				CheckDigests(CD_FINISH);
			}
			break;
		}
		if (writeReq->cmd == CHECKCREATE_TOPDIR) {
			// トップレベルディレクトリが存在しない場合は作成
			CheckAndCreateDestDir(dstBaseLen);
		}
		else if (writeReq->cmd == WRITE_FILE || writeReq->cmd == WRITE_BACKUP_FILE
		|| writeReq->cmd == CREATE_HARDLINK) {
			int		new_dst_len;
			if (writeReq->stat.renameCount == 0)
				new_dst_len = dir_len + wcscpyz(dst + dir_len, writeReq->stat.cFileName);
			else
				new_dst_len = dir_len + MakeRenameName(dst + dir_len,
										writeReq->stat.renameCount, writeReq->stat.cFileName);
			if (ApplyNasCompatName(dir_len, FALSE)) {
				new_dst_len = dir_len + (int)wcslen(dst + dir_len);
			}

			if (!isExec) {
				if (writeReq->cmd == CREATE_HARDLINK) {
					PutList(dst + dstPrefixLen, PL_HARDLINK);
					curTotal->linkFiles++;
				}
				else {
					PutList(dst + dstPrefixLen, IsReparseEx(writeReq->stat.dwFileAttributes) ?
						PL_REPARSE : PL_NORMAL, 0, writeReq->stat.WriteTime(),
						writeReq->stat.FileSize());
					curTotal->writeFiles++;
					curTotal->writeTrans += writeReq->stat.FileSize();
				}
				if (info.mode == MOVE_MODE) {
					SetFinishFileID(writeReq->stat.fileID, MoveObj::DONE);
				}
			}
			else if ((ret = WriteFileProc(new_dst_len)), isAbort) {
				break;
			}
		}
		else if (writeReq->cmd == CASE_CHANGE) {
			ret = CaseAlignProc(dir_len, true);
		}
		else if (writeReq->cmd == MKDIR || writeReq->cmd == INTODIR) {
			PushDepth(dst, dir_len);
			ret = WriteDirProc(dir_len);
		}
		else if (writeReq->cmd == DELETE_FILES) {	// stat is dest stat from SendRequest
			if (IsDir(writeReq->stat.dwFileAttributes)) {
				ret = DeleteDirProc(dst, dir_len, writeReq->stat.cFileName, &writeReq->stat,
									FR_MATCH);
			}
			else
				ret = DeleteFileProc(dst, dir_len, writeReq->stat.cFileName, &writeReq->stat);
		}
		else if (writeReq->cmd == RETURN_PARENT) {
			PopDepth(dst);
			break;
		}
		else {
			switch (writeReq->cmd) {
			case WRITE_ABORT: case WRITE_FILE_CONT:
			case WRITE_BACKUP_ACL: case WRITE_BACKUP_EADATA:
			case WRITE_BACKUP_ALTSTREAM: case WRITE_BACKUP_END:
				break;

			default:
				ret = FALSE;
				WCHAR cmd[2] = { WCHAR(writeReq->cmd + '0'), 0 };
				ConfirmErr(L"Illegal Request (internal error)", cmd, CEF_STOP|CEF_NOAPI);
				break;
			}
		}
	}
	return	ret && !isAbort;
}

BOOL FastCopy::CaseAlignProc(int dir_len, BOOL need_log)
{
	if (dir_len >= 0) {
		wcscpyz(dst + dir_len, writeReq->stat.cFileName);
	}

//	if (need_log) PutList(dst + dstPrefixLen, PL_CASECHANGE);

	if (!isExec) {
		return TRUE;
	}

	return	::MoveFileW(dst, dst);
}

BOOL FastCopy::WriteDirProc(int dir_len)
{
	BOOL		ret = TRUE;
	BOOL		is_mkdir = writeReq->cmd == MKDIR;
	BOOL		is_reparse = IsReparseEx(writeReq->stat.dwFileAttributes);
	DWORD		buf_size = writeReq->bufSize;
	int			new_dir_len;
	FileStat	sv_stat;

	memcpy(&sv_stat, &writeReq->stat, offsetof(FileStat, cFileName));

	if (writeReq->stat.renameCount == 0)
		new_dir_len = dir_len + wcscpyz(dst + dir_len, writeReq->stat.cFileName);
	else
		new_dir_len = dir_len + MakeRenameName(dst + dir_len,
								writeReq->stat.renameCount, writeReq->stat.cFileName, TRUE);
	if (ApplyNasCompatName(dir_len, TRUE)) {
		new_dir_len = dir_len + (int)wcslen(dst + dir_len);
	}

	if (buf_size) {
		if (dstDirExtBuf.RemainSize() < buf_size
		&& !dstDirExtBuf.Grow(ALIGN_SIZE(buf_size, MIN_DSTDIREXT_BUF))) {
			ConfirmErr(L"Can't alloc memory(dstDirExtBuf)", NULL, CEF_STOP);
			goto END;
		}
		memcpy(dstDirExtBuf.UsedEnd(), writeReq->buf, buf_size);
		sv_stat.acl = dstDirExtBuf.UsedEnd();
		sv_stat.ead = sv_stat.acl + sv_stat.aclSize;
		sv_stat.rep = sv_stat.ead + sv_stat.eadSize;
		dstDirExtBuf.AddUsedSize(buf_size);
	}

	if (is_mkdir) {
		if (!isExec || ::CreateDirectoryW(dst, NULL)) {
			if (isListing && !is_reparse)
				PutList(dst + dstPrefixLen, PL_DIRECTORY, 0, sv_stat.WriteTime());
			curTotal->writeDirs++;
		}
		else if (::GetLastError() != ERROR_ALREADY_EXISTS) {
			curTotal->wErrDirs++;
			ConfirmErr(L"CreateDirectory", dst + dstPrefixLen);
		}
	}
	wcscpy(dst + new_dir_len, L"\\");

	if (!is_reparse) {
		if ((ret = WriteProc(new_dir_len + 1)), isAbort) {	// 再帰
			goto END;
		}
	}

	dst[new_dir_len] = 0;	// 末尾の '\\' を取る

	if (ret && sv_stat.isCaseChanged) CaseAlignProc();

	// タイムスタンプ/ACL/属性/リパースポイントのセット
	if (!isExec || (ret = SetDirExtData(&sv_stat))) {
		if (isListing && is_reparse && is_mkdir) {
			PutList(dst + dstPrefixLen, PL_DIRECTORY|PL_REPARSE, 0, sv_stat.WriteTime());
		}
	}
	else if (is_reparse && is_mkdir) {
		// 新規作成dirのリパースポイント化に失敗した場合は、dir削除
		ForceRemoveDirectoryW(dst, info.aclReset|FMF_ATTR);
	}

	if (buf_size) {
		dstDirExtBuf.AddUsedSize(-(ssize_t)buf_size);
	}

END:
	return	ret && !isAbort;
}

BOOL FastCopy::SetDirExtData(FileStat *stat)
{
	HANDLE	fh;
	DWORD	mode = GENERIC_WRITE | (stat->acl && stat->aclSize && enableAcl ?
					(WRITE_OWNER|WRITE_DAC|acsSysSec) : 0) | READ_CONTROL;
	BOOL	is_reparse = IsReparseEx(stat->dwFileAttributes);
	BOOL	ret = FALSE;
	DWORD	share = FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
	DWORD	flg = FILE_FLAG_BACKUP_SEMANTICS|(is_reparse ? FILE_FLAG_OPEN_REPARSE_POINT : 0);

	//DebugW(L"CreateFile(SetDirExtData) %d %s\n", isExec, dst);

	fh = ForceCreateFileW(dst, mode, share, 0, OPEN_EXISTING, flg, 0, info.aclReset|FMF_ATTR);
	if (fh == INVALID_HANDLE_VALUE) {
		mode &= ~WRITE_OWNER;
		fh = ::CreateFileW(dst, mode, share, 0, OPEN_EXISTING, flg , 0);
	}
	if (fh == INVALID_HANDLE_VALUE) {
		if (is_reparse || (info.flags & REPORT_ACL_ERROR)) {
			ConfirmErr(is_reparse ? L"CreateDir(reparse)" : L"CreateDir(ACL)", dst + dstPrefixLen);
			curTotal->wErrDirs++;
		}
		goto END;
	}
	if (is_reparse && stat->rep && stat->repSize) {
		DynBuf	rp(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
		BOOL	is_set = TRUE;
		if (ReadReparsePoint(fh, rp, (DWORD)rp.Size()) > 0) {
			if (IsReparseDataSame(rp, stat->rep)) {
				is_set = FALSE;
			} else {
				DeleteReparsePoint(fh, rp);
			}
		}
		if (is_set) {
			if (WriteReparsePoint(fh, stat->rep, stat->repSize) <= 0) {
				curTotal->wErrDirs++;
				ConfirmErr(L"WriteReparsePoint(dir)", dst + dstPrefixLen);
				goto END;
			}
		}
		ret = TRUE; // リパースポイント作成が成功した場合は ACL成功の有無は問わない
	}

	if ((stat->acl && stat->aclSize) && (mode & WRITE_OWNER)) {
		void	*backupContent = NULL;
		DWORD	size;
		if (!::BackupWrite(fh, stat->acl, stat->aclSize, &size, FALSE, TRUE, &backupContent)) {
			if (info.flags & REPORT_ACL_ERROR)
				ConfirmErr(L"BackupWrite(DIRACL)", dst + dstPrefixLen);
			goto END;
		}
		::BackupWrite(fh, NULL, NULL, NULL, TRUE, TRUE, &backupContent);
	}
	ret = TRUE;

END:
	if (fh != INVALID_HANDLE_VALUE) {
		::SetFileTime(fh, &stat->ftCreationTime, &stat->ftLastAccessTime, &stat->ftLastWriteTime);
		::CloseHandle(fh);
	}
	if (stat->dwFileAttributes &
			(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_SYSTEM)) {
		::SetFileAttributesW(dst, stat->dwFileAttributes);
	}
	return	ret;
}

HANDLE FastCopy::CreateFileWithRetry(WCHAR *path, DWORD mode, DWORD share,
	SECURITY_ATTRIBUTES *sa, DWORD cr_mode, DWORD flg, HANDLE hTempl, int retry_max)
{
	HANDLE	fh = INVALID_HANDLE_VALUE;

	for (int i=0; i < retry_max && !isAbort; i++) {	// ウイルスチェックソフト対策
		if ((fh = ::CreateFileW(path, mode, share, sa, cr_mode, flg, hTempl))
				!= INVALID_HANDLE_VALUE)
			break;
		if (::GetLastError() != ERROR_SHARING_VIOLATION)
			break;
		::Sleep(i * i * 10);
		curTotal->openRetry++;
	}
	return	fh;
}

BOOL FastCopy::WriteFileWithReduce(HANDLE hFile, void *buf, DWORD size, OverLap *ovl)
{
	DWORD	total_trans = 0;
	DWORD	maxWriteSizeSv = maxWriteSize;
	BOOL	wait_ovl = FALSE;
	BOOL	ret = TRUE;

	ovl->waiting   = false;
	ovl->orderSize = size;
	ovl->transSize = 0;

	while (total_trans < size) {
		DWORD io_size = min(size - total_trans, maxWriteSize);
		DWORD trans = 0;

		if (::WriteFile(hFile, (BYTE *)buf + total_trans, io_size, &trans, &ovl->ovl)) {
			total_trans += trans;
			ovl->SetOvlOffset(ovl->GetOvlOffset() + trans);
			continue;
		}
		DWORD	err = ::GetLastError();
		if (err == ERROR_NO_SYSTEM_RESOURCES) {
			if (min(size, maxWriteSize) <= REDUCE_SIZE) {
				ret = FALSE;
				break;
			}
			maxWriteSize -= REDUCE_SIZE;
			maxWriteSize = ALIGN_SIZE(maxWriteSize, REDUCE_SIZE);
			wait_ovl = TRUE;
			continue;
		}
		if (err == ERROR_IO_PENDING) {
			if (wait_ovl) {
				if (!WaitOverlapped(hFile, ovl)) return FALSE;
				total_trans += trans;
				ovl->SetOvlOffset(ovl->GetOvlOffset() + trans);
			}
			else {
				ovl->orderSize = io_size;
				ovl->waiting   = true;
				break;	// success overlapped request
			}
		}
		else {
			ret = FALSE;
			break;
		}
	}
	if (!ovl->waiting) {
		if (total_trans == size) {
			ovl->transSize = total_trans;
			ovl->orderSize = size;
		}
		else ret = FALSE;
	}

	if (maxWriteSize != maxWriteSizeSv) {
		WCHAR buf[128];
		swprintf(buf, FMT_REDUCEMSG, 'W', maxWriteSizeSv / 1024/1024, maxWriteSize / 1024/1024);
		WriteErrLog(buf);
	}
	return	ret;
}

BOOL FastCopy::RestoreHardLinkInfo(DWORD *link_data, WCHAR *path, int base_len)
{
	LinkObj	*obj;
	DWORD	hash_id = hardLinkList.MakeHashId(link_data);

	if (!(obj = (LinkObj *)hardLinkList.Search(link_data, hash_id))) {
		ConfirmErr(L"HardLinkInfo is gone(internal error)", path + base_len, CEF_NOAPI);
		return	FALSE;
	}

	wcscpy(path + base_len, obj->path);

	if (--obj->nLinks <= 1) {
		hardLinkList.UnRegister(obj);
		delete obj;
	}

	return	TRUE;
}

BOOL FastCopy::ApplyNasCompatName(int dir_len, BOOL is_dir)
{
	if (!IsNetFs(dstFsType) || dir_len < 0) {
		return FALSE;
	}

	WCHAR *leaf = dst + dir_len;
	if (!*leaf || *leaf == L':') { // Skip ADS/empty name
		return FALSE;
	}

	WCHAR org_name[MAX_PATH];
	WCHAR base_name[MAX_PATH];
	WCHAR new_name[MAX_PATH];

	wcscpyz(org_name, leaf);
	MakeNasCompatBaseName(org_name, base_name, _countof(base_name), is_dir);
	// NAS ファイルシステム (ext4/btrfs) の UTF-8 バイト上限に切り詰め
	// 衝突サフィックス (_NNNN) 用に 10 バイト分のマージンを確保
	TruncateToNasUtf8Limit(base_name, NAS_MAX_FILENAME_UTF8_BYTES - 10, is_dir);
	wcscpyz(new_name, base_name);

	BOOL changed = wcscmp(org_name, new_name) != 0;
	if (changed) {
		wcscpyz(leaf, new_name);
	}

	if (changed) {
		DWORD attr = ::GetFileAttributesW(dst);
		BOOL is_existing_dir = (attr != 0xffffffff) && (attr & FILE_ATTRIBUTE_DIRECTORY);

		if (!(is_dir && is_existing_dir)) {
			for (int i=1; attr != 0xffffffff && i <= 9999; i++) {
				if (!MakeNasCompatCollisionName(base_name, i, is_dir, new_name, _countof(new_name))) {
					break;
				}
				TruncateToNasUtf8Limit(new_name, NAS_MAX_FILENAME_UTF8_BYTES, is_dir);
				wcscpyz(leaf, new_name);
				attr = ::GetFileAttributesW(dst);
			}
		}
	}

	if (changed) {
		WCHAR msg_buf[MAX_PATH_EX];
		swprintf(msg_buf, L"NAS name normalized: %s -> %s", org_name, leaf);
		WriteErrLog(msg_buf);
		if (isListing) {
			PutList(msg_buf, PL_ERRMSG);
		}
	}
	return changed;
}

BOOL FastCopy::WriteDigestProc(int dst_len, FileStat *stat, DigestObj::Status status)
{
	int	path_len = dst_len + 1;

	DataList::Head *head = digestList.Alloc(NULL, 0, sizeof(DigestObj) + path_len * sizeof(WCHAR));
	if (!head) {
		ConfirmErr(L"Can't alloc memory(digestList)", NULL, CEF_STOP);
		return FALSE;
	}
	DigestObj *obj = (DigestObj *)head->data;
	obj->fileID = stat->fileID;
	obj->fileSize = (status == DigestObj::PASS) ? 0 : stat->FileSize();
	obj->wTime = stat->WriteTime();
	obj->dwAttr = stat->dwFileAttributes;
	obj->status = status;
	obj->pathLen = path_len;

	// writeReq will be chaned in big file
	memcpy(obj->digest, writeReq->stat.digest, dstDigest.GetDigestSize());
	memcpy(obj->path, dst, path_len * sizeof(WCHAR));

	BOOL is_empty_buf = digestList.RemainSize() <= digestList.MinMargin();
	if (is_empty_buf || (info.flags & SERIAL_VERIFY_MOVE)) {
		CheckDigests(is_empty_buf ? CD_WAIT : CD_NOWAIT); // empty なら wait
	}
	return TRUE;
}

BOOL FastCopy::WriteFileProcCore(HANDLE *_fh, FileStat *stat, WInfo *_wi)
{
	HANDLE	&fh = *_fh;
	WInfo	&wi = *_wi;
	DWORD	mode = GENERIC_WRITE;
	DWORD	share = FILE_SHARE_READ|FILE_SHARE_WRITE;
	DWORD	flg = (wi.is_nonbuf ? FILE_FLAG_NO_BUFFERING : 0) | FILE_FLAG_SEQUENTIAL_SCAN
					| (enableBackupPriv ? FILE_FLAG_BACKUP_SEMANTICS : 0);
	BOOL	useOvl = (flagOvl && info.IsMinOvl(wi.file_size)) &&
		((info.flags & NET_BKUPWR_NOOVL) == 0 || IsLocalFs(dstFsType) || wi.cmd == WRITE_FILE);

	if (wi.cmd == WRITE_BACKUP_FILE || wi.cmd == WRITE_BACKUP_ALTSTREAM) {
		if (stat->acl && stat->aclSize && enableAcl) {
			mode |= WRITE_OWNER|WRITE_DAC|acsSysSec;
		}
	}

	if (useOvl /* && wi.cmd != WRITE_BACKUP_FILE*/) {
		flg |= flagOvl;		// BackupWrite しないなら OVERLAPPED モードで開く
	}
	if (wi.is_reparse) {
		flg |= FILE_FLAG_OPEN_REPARSE_POINT;
	}

	//DebugW(L"CreateFile(WriteFileProcCore) %d %s\n", isExec, dst);
	fh = ForceCreateFileW(dst, mode, share, 0, CREATE_ALWAYS, flg, 0, FMF_ATTR);
	if (fh == INVALID_HANDLE_VALUE) {
		if (info.aclReset || enableAcl) {
			fh = ForceCreateFileW(dst, mode, share, 0, CREATE_ALWAYS, flg, 0,
				FMF_ACL|info.aclReset);
		}
	}
	if (fh == INVALID_HANDLE_VALUE) {
		if (!wi.is_stream || (info.flags & REPORT_STREAM_ERROR))
			ConfirmErr(wi.is_stream ? L"CreateFile(st)" : L"CreateFile", dst + dstPrefixLen);
		return	FALSE;
	}

	BOOL	ret = TRUE;
	if (wi.is_reparse) {
		if (WriteReparsePoint(fh, writeReq->buf, stat->repSize) <= 0) {
			ret = FALSE;
			ConfirmErr(L"WriteReparsePoint(File)", dst + dstPrefixLen);
		}
	} else {
		HANDLE	hIoFile = fh;
		if (useOvl && (flg & flagOvl) == 0) {
			hIoFile = ::CreateFileW(dst, mode, share, 0, CREATE_ALWAYS, flg|flagOvl, 0);
			if (hIoFile == INVALID_HANDLE_VALUE) hIoFile = fh;
		}
		if (IsNetFs(dstFsType)) {
			DisableLocalBuffering(hIoFile);
		}
		ret = WriteFileCore(hIoFile, stat, &wi, mode, share, flg);

		if (useOvl && hIoFile != fh && hIoFile != INVALID_HANDLE_VALUE) {
			::CloseHandle(hIoFile);
		}
	}
	return	ret;
}

BOOL FastCopy::WriteFileProc(int dst_len)
{
	HANDLE		fh	= INVALID_HANDLE_VALUE;
	BOOL		ret = TRUE;
	WInfo		wi;
	FileStat	*stat = &writeReq->stat, sv_stat;

	wi.file_size = writeReq->stat.FileSize();
	wi.cmd	= writeReq->cmd;
	wi.is_reparse = IsReparseEx(stat->dwFileAttributes);
	wi.is_hardlink = wi.cmd == CREATE_HARDLINK;
	wi.is_stream = wi.cmd == WRITE_BACKUP_ALTSTREAM;
	BOOL	use_wcache = (info.flags & USE_OSCACHE_WRITE) ? TRUE : FALSE;
	wi.is_nonbuf = (wi.file_size >= nbMinSize && !use_wcache && !wi.is_reparse)
		&& (IsLocalFs(dstFsType) /*|| wi.file_size >= NETDIRECT_SIZE*/) ? TRUE : FALSE;

	BOOL	is_require_del = (stat->isNeedDel || (info.flags & (DEL_BEFORE_CREATE
		|((info.flags & BY_ALWAYS) ? REPARSE_AS_NORMAL : 0))) || UseHardlink()) ? TRUE : FALSE;

	// writeReq の stat を待避して、それを利用する
	if (wi.cmd == WRITE_BACKUP_FILE || wi.file_size > writeReq->bufSize) {
		memcpy((stat = &sv_stat), &writeReq->stat, offsetof(FileStat, cFileName));
	}
	WaitCheck();

	if (is_require_del) {
		//DebugW(L"DeleteFileW(WriteFileProc) %d %s\n", isExec, dst);
		ForceDeleteFileW(dst, FMF_ATTR|info.aclReset);
	}
	if (wi.is_hardlink) {
		if ((ret = RestoreHardLinkInfo(writeReq->stat.GetLinkData(), hardLinkDst, dstBaseLen))) {
			if (!(ret = ::CreateHardLinkW(dst, hardLinkDst, NULL))) {
				ConfirmErr(L"CreateHardLink", dst + dstPrefixLen);
			}
			stat->dwFileAttributes = 0;	// mark for hardlink
			stat->FileSize() = 0;
		}
	}
	else {
		ret = WriteFileProcCore(&fh, stat, &wi);
		if (!ret) SetErrWFileID(stat->fileID);
	}
	if (IsUsingDigestList() && !wi.is_stream && !isAbort) {	// digestList に error を含めて登録
		if (!WriteDigestProc(dst_len, stat,
			ret ? (wi.is_reparse ? DigestObj::PASS : DigestObj::OK) : DigestObj::NG)) {
			ret = FALSE;	 // false になるのはABORTレベル
		}
	}
	if (wi.cmd == WRITE_BACKUP_FILE) {
		/* ret = */ WriteFileBackupProc(fh, dst_len);	// ACL/EADATA/STREAM エラーは無視
	}
	if (!wi.is_hardlink) {
		if (ret && !wi.is_stream) {
			::SetFileTime(fh, &stat->ftCreationTime, &stat->ftLastAccessTime,
				&stat->ftLastWriteTime);
		}
		::CloseHandle(fh);

		if (IsWebDAV(dstFsType)) {	// WebDAV は CloseHandle後に SetFileTime しないと反映されない
			fh = CreateFileW(dst, GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
			::SetFileTime(fh, &stat->ftCreationTime, &stat->ftLastAccessTime,
					&stat->ftLastWriteTime);
			::CloseHandle(fh);
		}
	}

	if (ret) {
		if (!wi.is_stream) {
			if (stat->isCaseChanged && !is_require_del) CaseAlignProc();
			if (stat->dwFileAttributes /* &
					(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_SYSTEM) */) {
				::SetFileAttributesW(dst, stat->dwFileAttributes);
			}
		}
		int	&totalFiles = wi.is_hardlink ? curTotal->linkFiles :
							wi.is_stream ? curTotal->writeAclStream : curTotal->writeFiles;
		totalFiles++;
	}
	else {
		SetTotalErrInfo(wi.is_stream, wi.file_size, TRUE);
		SetErrWFileID(stat->fileID);
		if (!wi.is_stream && // fh が無効かつERROR_NO_SYSTEM_RESOURCESはネットワーク作成後エラー
			(fh != INVALID_HANDLE_VALUE || ::GetLastError() == ERROR_NO_SYSTEM_RESOURCES)) {
			//DebugW(L"DeleteFileW(WriteFileProc) %d %s\n", isExec, dst);
			ForceDeleteFileW(dst, FMF_ATTR|info.aclReset);
		}
	}
	if (!wi.is_stream && info.mode == MOVE_MODE && (info.verifyFlags & VERIFY_FILE) == 0 &&
		!isAbort) {
		SetFinishFileID(stat->fileID, ret ? MoveObj::DONE : MoveObj::ERR);
	}
	if ((!isExec || (isListing && !IsUsingDigestList())) && !wi.is_stream && ret) {
		DWORD flags = wi.is_hardlink ? PL_HARDLINK : wi.is_reparse ? PL_REPARSE : PL_NORMAL;
		PutList(dst + dstPrefixLen, flags, 0, stat->WriteTime(), stat->FileSize());
	}

	return	ret && !isAbort;
}

BOOL FastCopy::WriteFileCore(HANDLE fh, FileStat *stat, WInfo *_wi, DWORD mode, DWORD share,
	DWORD flg)
{
	BOOL	ret = TRUE;
	HANDLE	fh2 = INVALID_HANDLE_VALUE;
	WInfo	&wi = *_wi;
	int64	file_size = wi.file_size;
	int64	total_size = 0;
	int64	order_total = 0;
	BOOL	is_reopen = wi.is_nonbuf && (file_size % dstSectorSize);

	if (wOvl.TopObj(USED_LIST)) {
		ConfirmErr(L"Not clear wOvl in WriteFileCore", NULL, CEF_STOP);
		return FALSE;
	}
	if (is_reopen) {
		flg &= ~FILE_FLAG_NO_BUFFERING;
		//DebugW(L"CreateFile(WriteFileCore) %d %s\n", isExec, dst);
		fh2 = ::CreateFileW(dst, mode, share, 0, OPEN_EXISTING, flg, 0);
	}
	if (file_size > writeReq->readSize) {	// ファイルブロックをなるべく連続確保する
		int64	alloc_size = wi.is_nonbuf ? ALIGN_SIZE(file_size, dstSectorSize) : file_size;
		LONG	high_size = (LONG)(alloc_size >> 32);

		::SetFilePointer(fh, (LONG)alloc_size, &high_size, FILE_BEGIN);
		if (!::SetEndOfFile(fh) && ::GetLastError() == ERROR_DISK_FULL) {
			ConfirmErr(wi.is_stream ? L"SetEndOfFile(st)" : L"SetEndOfFile",
			 dst + dstPrefixLen, file_size >= info.allowContFsize ? 0 : CEF_STOP);
			ret = FALSE;
			goto END;
		}
		::SetFilePointer(fh, 0, NULL, FILE_BEGIN);
	}
	while (total_size < file_size) {
		DWORD	trans = 0;
		DWORD	write_size = writeReq->readSize;

		if (write_size + total_size > file_size) {
			ConfirmErr(L"Internal Error(write)", dst+dstPrefixLen, CEF_STOP);
			break;
		}
		OverLap	*ovl = wOvl.GetObj(FREE_LIST);
		wOvl.PutObj(USED_LIST, ovl);
		ovl->SetOvlOffset(total_size + order_total);

		if (wi.is_nonbuf) write_size = ALIGN_SIZE(write_size, dstSectorSize);
		ret = WriteFileWithReduce(fh, writeReq->buf, write_size, ovl);
		WaitCheck();

		if (!ret || (!ovl->waiting && ovl->transSize == 0)) {
			ret = FALSE;
			if (!wi.is_stream || (info.flags & REPORT_STREAM_ERROR)) {
				ConfirmErr(wi.is_stream ? L"WriteFile(st)" : L"WriteFile", dst + dstPrefixLen,
					(::GetLastError() != ERROR_DISK_FULL || file_size >= info.allowContFsize
					 || wi.is_stream) ? 0 : CEF_STOP);
			}
			break;
		}
		order_total += ovl->orderSize;

		BOOL is_empty_ovl = wOvl.IsEmpty(FREE_LIST);
		BOOL is_flush = (total_size + order_total >= file_size) || writeInterrupt
						|| !ovl->waiting || writeReq->isFlush;
		if (is_empty_ovl || is_flush || waitLv) {
			while (OverLap	*ovl_tmp = wOvl.GetObj(USED_LIST)) {
				wOvl.PutObj(FREE_LIST, ovl_tmp);
				if (!(ret = WaitOvlIo(fh, ovl_tmp, &total_size, &order_total))) {
					if (!wi.is_stream || (info.flags & REPORT_STREAM_ERROR)) {
						ConfirmErr(wi.is_stream ? L"WriteFileWait(st)" : L"WriteFileWait",
							dst + dstPrefixLen);
					}
					break;
				}
				curTotal->writeTrans += ovl_tmp->transSize;
				WaitCheck();
				if ((!is_flush && !waitLv) || isAbort) break;
			}
		}
		if (!ret || isAbort) break;
		if (total_size < file_size) {	// 続きがある
			if (!RecvRequest(INT_RDC(wOvl.TopObj(USED_LIST)), is_empty_ovl)
			|| writeReq->cmd != WRITE_FILE_CONT) {
				ret = FALSE;
				total_size = file_size;
				if (!isAbort && writeReq->cmd != WRITE_ABORT) {
					WCHAR cmd[2] = { WCHAR(writeReq->cmd + '0'), 0 };
					ConfirmErr(L"Illegal Request2 (internal error)", cmd, CEF_STOP|CEF_NOAPI);
				}
				break;
			}
		} else {
			curTotal->writeTrans -= (total_size - file_size); // truncate予定分を引く
			break;
		}
	}
	if (ret && is_reopen && fh2 == INVALID_HANDLE_VALUE) {
		//DebugW(L"CreateFile(WriteFileCore2) %d %s\n", isExec, dst);
		fh2 = CreateFileWithRetry(dst, mode, share, 0, OPEN_EXISTING, flg, 0, 10);
		if (fh2 == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_SHARING_VIOLATION) {
			mode &= ~(WRITE_OWNER|WRITE_DAC|acsSysSec);
			fh2 = CreateFileWithRetry(dst, mode, share, 0, OPEN_EXISTING, flg, 0, 10);
		}
		if (fh2 == INVALID_HANDLE_VALUE) {
			ret = FALSE;
			if (!wi.is_stream || (info.flags & REPORT_STREAM_ERROR))
				ConfirmErr(wi.is_stream ? L"CreateFile2(st)" : L"CreateFile2", dst +dstPrefixLen);
		}
	}
	if (ret && total_size != file_size) {
		::SetFilePointer(fh2, stat->nFileSizeLow, (LONG *)&stat->nFileSizeHigh, FILE_BEGIN);
		if (!(ret = ::SetEndOfFile(fh2))) {
			if (!wi.is_stream || (info.flags & REPORT_STREAM_ERROR))
				ConfirmErr(wi.is_stream ? L"SetEndOfFile2(st)" : L"SetEndOfFile2",
					dst + dstPrefixLen);
		}
	}
END:
	if (!ret) {
		IoAbortFile(fh, &wOvl);
	}
	if (fh2 != INVALID_HANDLE_VALUE) ::CloseHandle(fh2);
	return	ret && !isAbort;
}

BOOL FastCopy::WriteFileBackupProc(HANDLE fh, int dst_len)
{
	BOOL	ret = TRUE, is_continue = TRUE;
	DWORD	size;
	void	*backupContent = NULL;	// for BackupWrite

	while (!isAbort && is_continue) {
		if (!RecvRequest()) {
			ret = FALSE;
			if (!isAbort) {
				WCHAR cmd[2] = { WCHAR(writeReq->cmd + '0'), 0 };
				ConfirmErr(L"Illegal Request3 (internal error)", cmd, CEF_STOP|CEF_NOAPI);
			}
			break;
		}
		switch (writeReq->cmd) {
		case WRITE_BACKUP_ACL: case WRITE_BACKUP_EADATA:
			SetLastError(0);
			if (!(ret = ::BackupWrite(fh, writeReq->buf, writeReq->cmd == WRITE_BACKUP_ACL ?
				writeReq->stat.aclSize : writeReq->stat.eadSize, &size, FALSE, TRUE,
				&backupContent))) {
				if (info.flags & REPORT_ACL_ERROR)
					ConfirmErr(L"BackupWrite(ACL/EADATA)", dst + dstPrefixLen);
			}
			break;

		case WRITE_BACKUP_ALTSTREAM:
			wcscpy(dst + dst_len, writeReq->stat.cFileName);
			ret = WriteFileProc(0);
			dst[dst_len] = 0;
			break;

		case WRITE_BACKUP_END:
			is_continue = FALSE;
			break;

		case WRITE_FILE_CONT:	// エラー時のみ
			break;

		case WRITE_ABORT:
			ret = FALSE;
			break;

		default:
			if (!isAbort) {
				WCHAR cmd[2] = { WCHAR(writeReq->cmd + '0'), 0 };
				ConfirmErr(L"Illegal Request4 (internal error)", cmd, CEF_STOP|CEF_NOAPI);
			}
			ret = FALSE;
			break;
		}
	}

	if (backupContent) {
		::BackupWrite(fh, NULL, NULL, NULL, TRUE, TRUE, &backupContent);
	}
	return	ret && !isAbort;
}

BOOL FastCopy::CheckDigests(CheckDigestMode mode)
{
	DataList::Head *head;
	BOOL			ret = TRUE;

	if (mode == CD_NOWAIT && !digestList.Peek()) return ret;

	while (!isAbort && (head = digestList.Get())) {
		if (!MakeDigestAsync((DigestObj *)head->data)) {
			ret = FALSE;
		}
	}
	digestList.Clear();

	cv.Lock();
	runMode = (mode == CD_FINISH) ? RUN_FINISH : RUN_NORMAL;
	cv.Notify();
	cv.UnLock();

	if (mode == CD_FINISH) {
		DigestCalc	*calc = GetDigestCalc(NULL, 0);
		if (calc) {
			PutDigestCalc(calc, DigestCalc::FIN);
		}
	}

	if (mode == CD_WAIT || mode == CD_FINISH) {
		wDigestList.Lock();
		while (wDigestList.Num() > 0 && !isAbort) {
			CvWait(wDigestList.GetCv(), CV_WAIT_TICK);
		}
		wDigestList.UnLock();
	}

	return	ret && !isAbort;
}

// RecvRequest()
//  keepCur:   現在発行中の writeReq を writeWaitList に移動して解放を延期する
//  なお、keepCur==FALSE の場合、滞留writeWaitListがあれば全開放する
BOOL FastCopy::RecvRequest(BOOL keepCur, BOOL freeLast)
{
	cv.Lock();

	if (!keepCur || freeLast) { // !keepCur == clear all
		while (ReqHead *req = writeWaitList.TopObj()) {
			writeWaitList.DelObj(req);
			WriteReqDone(req);
			if (keepCur && freeLast) break;
		}
	}
	if (writeReq) {
		writeReqList.DelObj(writeReq);
		if (keepCur) {
			writeWaitList.AddObj(writeReq);
		} else {
			WriteReqDone(writeReq);
		}
		writeReq = NULL;
	}

	CheckDstRequest();
	BOOL	is_serial_mv = (info.flags & SERIAL_VERIFY_MOVE);

	while (writeReqList.IsEmpty() && !isAbort) {
		if (info.mode == MOVE_MODE && (info.verifyFlags & VERIFY_FILE)
			&& writeWaitList.IsEmpty()) {
			if (runMode == RUN_DIGESTREQ || (is_serial_mv && digestList.Num() > 0)) {
				cv.UnLock();
				CheckDigests(CD_WAIT);
				cv.Lock();
				continue;
			}
		}
		CvWait(&cv, CV_WAIT_TICK);
		CheckDstRequest();
	}
	writeReq = writeReqList.TopObj();
	writeInterrupt = (isSameDrv && (writeReq && !writeReqList.NextObj(writeReq))
		|| (runMode == RUN_DIGESTREQ || (is_serial_mv && digestList.Num() > 0))) ? TRUE : FALSE;
	// DebugW(L"RecvRequest(%x) done(%d) %.254s\n", writeReq ? writeReq->cmd : 0, isExec, writeReq ? writeReq->stat.cFileName : L"");

	cv.UnLock();

	return	writeReq && !isAbort;
}

void FastCopy::WriteReqDone(ReqHead *req)
{
	freeOffset = (BYTE *)req + req->reqSize;
	if (freeOffset == usedOffset) {
//		Debug("reached\n");
		usedOffset = freeOffset = mainBuf.Buf();
	}

	if (!isSameDrv || (writeReqList.IsEmpty() && writeWaitList.IsEmpty())) {
		cv.Notify();
	}
}

BOOL FastCopy::MakeDigestAsync(DigestObj *obj)
{
	BOOL	useOvl = flagOvl && info.IsMinOvl(obj->fileSize);
	DWORD	flg = ((info.flags & USE_OSCACHE_READ) ? 0 : FILE_FLAG_NO_BUFFERING)
				| FILE_FLAG_SEQUENTIAL_SCAN | (useOvl ? flagOvl : 0)
				| (enableBackupPriv ? FILE_FLAG_BACKUP_SEMANTICS : 0);
	DWORD	share = FILE_SHARE_READ|FILE_SHARE_WRITE;
	HANDLE	hFile = INVALID_HANDLE_VALUE;
	int64	file_size = 0;

	if (wOvl.TopObj(USED_LIST)) {
		ConfirmErr(L"Not clear wOvl in MakeDigestAsync", NULL, CEF_STOP);
		return FALSE;
	}
	WaitCheck();

	if (obj->status == DigestObj::NG) goto ERR;

	if (obj->fileSize) {
		hFile = CreateFileWithRetry(obj->path, GENERIC_READ, share, 0, OPEN_EXISTING, flg, 0, 5);
		if (hFile == INVALID_HANDLE_VALUE) {
			ConfirmErr(L"OpenFile(digest)", obj->path + dstPrefixLen);
			goto ERR;
		}
		::GetFileSizeEx(hFile, (LARGE_INTEGER *)&file_size);
	}
	if (file_size != obj->fileSize) {
		ConfirmErr(L"Size is changed", obj->path + dstPrefixLen, CEF_NOAPI);
		goto ERR;
	}
	if (obj->fileSize == 0) {
		if (DigestCalc	*calc = GetDigestCalc(obj, 0)) {
			calc->dataSize = 0;
			PutDigestCalc(calc, DigestCalc::DONE);
		}
		goto END;
	}
	int64	total_size  = 0;
	int64	order_total = 0;

	while (total_size < file_size && !isAbort) {
		DWORD	max_trans = TransSize(maxDigestReadSize);
		if (useOvl && !info.IsNormalOvl(file_size)) {
			max_trans = info.GenOvlSize(file_size);
		}
		int64	remain = file_size - total_size;
		DWORD	io_size = DWORD((remain > max_trans) ? max_trans : remain);
		io_size = ALIGN_SIZE(io_size, dstSectorSize);
		BOOL	ret = TRUE;

		OverLap	*ovl = wOvl.GetObj(FREE_LIST);
		wOvl.PutObj(USED_LIST, ovl);
		ovl->SetOvlOffset(total_size + order_total);
		if (!(ovl->calc = GetDigestCalc(obj, io_size)) || isAbort) break;

		if (!(ret = ReadFileWithReduce(hFile, ovl->calc->data, io_size, ovl))) {
			ConfirmErr(L"ReadFile(digest)", obj->path + dstPrefixLen);
			break;
		}
		order_total += ovl->orderSize;
		BOOL is_empty_ovl = wOvl.IsEmpty(FREE_LIST);
		BOOL is_flush = (total_size + order_total >= file_size) || writeInterrupt || !ovl->waiting;
		if (!is_empty_ovl && !is_flush) continue;

		while (OverLap	*ovl_tmp = wOvl.GetObj(USED_LIST)) {
			wOvl.PutObj(FREE_LIST, ovl_tmp);
			if (!(ret = WaitOvlIo(hFile, ovl_tmp, &total_size, &order_total))) {
				if (ovl_tmp->calc) PutDigestCalc(ovl_tmp->calc, DigestCalc::PASS);
				ConfirmErr(L"ReadFile(digest2)", obj->path + dstPrefixLen);
				break;
			}
			ovl_tmp->calc->dataSize = ovl_tmp->transSize;
			curTotal->verifyTrans += ovl_tmp->transSize;
			PutDigestCalc(ovl_tmp->calc, total_size >= file_size ? DigestCalc::DONE
				: DigestCalc::CONT);
			WaitCheck();
			if (!ret || (!is_flush && !waitLv) || isAbort) break;
		}
		if (!ret || isAbort) break;
	}
	if (total_size < file_size) goto ERR;

END:
	if (hFile != INVALID_HANDLE_VALUE) ::CloseHandle(hFile);
	return TRUE;

ERR:
	PutDigestCalc(GetDigestCalc(obj, 0), (obj->status == DigestObj::NG) ?
		DigestCalc::PRE_ERR : DigestCalc::ERR);
	if (hFile != INVALID_HANDLE_VALUE) {
		if (wOvl.TopObj(USED_LIST)) {
			for (OverLap *ovl=wOvl.TopObj(USED_LIST); ovl; ovl=wOvl.NextObj(USED_LIST, ovl)) {
				if (ovl->calc) PutDigestCalc(ovl->calc, DigestCalc::PASS);
			}
		}
		IoAbortFile(hFile, &wOvl);
		::CloseHandle(hFile);
	}
	return	FALSE;
}

