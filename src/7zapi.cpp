// ============================================================================
//  7zpack —— 7-Zip(zstd) 的扁平 C 接口封装
//
//  依附 7z.dll 的 COM 接口：IOutArchive / IInArchive / ISetProperties。
//  对外只暴露几个 __stdcall 函数，供 AHK 等脚本语言直接 DllCall。
//
//  构建方式见 build.sh：本文件会被拼接到 7-Zip 官方 Client7z.cpp 的类定义
//  之后一起编译，所以可以直接复用 Client7z 里已经写好的各种回调类。
// ============================================================================

#include <oleauto.h>        // SysStringByteLen：格式 CLSID 是以「二进制 BSTR」形式报出来的

// ---------------------------- 对外接口 --------------------------------------
struct PACK_OPTS
{
  int Method;        // 0=LZMA2 1=ZSTD 2=BROTLI 3=LZ4 4=LZ5 5=LIZARD 6=COPY
  int Level;         // 0..9 ；-1 = 用默认
  int Threads;       // 0 = 自动
  int Solid;         // -1 = 默认, 0 = 关, 1 = 开
  int ZstdMax;       // 1 = zstd --max (mcmilk 扩展, 最高级别)
  int ShowProgress;  // 1 = 在 DLL 内显示进度窗口；0 = 静默（旧调用方全零，天然兼容）
  const wchar_t *Password;   // NULL = 不加密
};

enum
{
  PK_OK        = 0,
  PK_E_PARAM   = 1,
  PK_E_DLL     = 2,   // 找不到/加载不了 7z.dll
  PK_E_SRC     = 3,   // 源路径无效
  PK_E_DEST    = 4,   // 目标不可写
  PK_E_CREATE  = 5,   // CreateObject 失败
  PK_E_PROP    = 6,   // SetProperties 失败(方法名/级别不被接受)
  PK_E_UPDATE  = 7,   // 压缩失败
  PK_E_PARTIAL = 8,   // 部分文件读不了(其余已压入)
  PK_E_OPENARC = 9,   // 打不开归档(损坏/密码)
  PK_E_EXTRACT = 10,  // 解压失败
  PK_E_CANCEL  = 11,  // 用户点了取消
  PK_E_FORMAT  = 12   // 归档扩展名与「只写 7z 容器」冲突
};

static UString g_Err;
static volatile LONG g_CancelReq = 0;   // 进度窗口点「取消」后置 1，回调据此中断
static HMODULE g_h7z;
static Func_CreateObject g_CreateObject;
static Func_GetNumberOfFormats g_GetNumberOfFormats;
static Func_GetHandlerProperty2 g_GetHandlerProperty2;

static void SetErr(const wchar_t *s)          { g_Err = s; }
static void SetErr2(const wchar_t *s, const UString &tail)
{
  g_Err = s; g_Err += tail;
}

static bool Load7zModule()
{
  if (g_CreateObject)
    return true;
  wchar_t path[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(g_hInstance, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return false;
  while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/')
    n--;
  if (n + 9 >= MAX_PATH)
    return false;
  wcscpy(path + n, L"7z.dll");
  g_h7z = ::LoadLibraryW(path);
  if (!g_h7z)
    return false;
  g_CreateObject = Z7_GET_PROC_ADDRESS(Func_CreateObject, g_h7z, "CreateObject");
  g_GetNumberOfFormats = Z7_GET_PROC_ADDRESS(Func_GetNumberOfFormats, g_h7z, "GetNumberOfFormats");
  g_GetHandlerProperty2 = Z7_GET_PROC_ADDRESS(Func_GetHandlerProperty2, g_h7z, "GetHandlerProperty2");
  return g_CreateObject != NULL;
}

// ---------------------- 归档格式表（解压用） --------------------------------
// 每种格式在 7z.dll 里都是一个独立的 handler（各自的 CLSID）。
// 只用 7z 的 CLSID 去开 zip 必然失败 —— 这就是「7-Zip 能解、本 DLL 报不是有效 7z 归档」的原因。
// 7-Zip 自己的界面也是拿 GetHandlerProperty2 把格式列出来，再挨个试着打开。
struct CFormatEntry
{
  CLSID   Clsid;
  UString Ext;        // 该格式的扩展名，空格分隔，例如 "zip 001"
  bool    HasSig;     // 有没有文件头特征（tar / brotli 这类没有）
};

static CObjectVector<CFormatEntry> g_Formats;
static bool g_FormatsLoaded = false;

static bool LoadFormats()
{
  if (g_FormatsLoaded)
    return !g_Formats.IsEmpty();
  g_FormatsLoaded = true;
  if (!g_GetNumberOfFormats || !g_GetHandlerProperty2)
    return false;

  UInt32 n = 0;
  if (g_GetNumberOfFormats(&n) != S_OK)
    return false;

  for (UInt32 i = 0; i < n; i++)
  {
    NCOM::CPropVariant cls;
    if (g_GetHandlerProperty2(i, NArchive::NHandlerPropID::kClassID, &cls) != S_OK)
      continue;
    if (cls.vt != VT_BSTR || !cls.bstrVal)
      continue;
    // 注意：这里报的是「二进制 GUID」而不是字符串，16 个字节直接就是 CLSID
    if (::SysStringByteLen(cls.bstrVal) != sizeof(GUID))
      continue;
    CLSID id = *(const GUID *)(const void *)cls.bstrVal;

    CFormatEntry e;
    e.Clsid = id;
    e.HasSig = false;
    NCOM::CPropVariant ext;
    if (g_GetHandlerProperty2(i, NArchive::NHandlerPropID::kExtension, &ext) == S_OK
        && ext.vt == VT_BSTR && ext.bstrVal)
      e.Ext = ext.bstrVal;

    // 有文件头的格式才能拿内容去猜；没有文件头的（brotli / tar）只认扩展名，
    // 否则它会把任意垃圾字节都当成自己的数据，报出来的错就成了「解压失败」而不是「不是压缩包」
    NCOM::CPropVariant sig;
    if (g_GetHandlerProperty2(i, NArchive::NHandlerPropID::kSignature, &sig) == S_OK
        && sig.vt == VT_BSTR && sig.bstrVal && ::SysStringByteLen(sig.bstrVal) > 0)
      e.HasSig = true;
    else
    {
      NCOM::CPropVariant multi;
      if (g_GetHandlerProperty2(i, NArchive::NHandlerPropID::kMultiSignature, &multi) == S_OK
          && multi.vt == VT_BSTR && multi.bstrVal && ::SysStringByteLen(multi.bstrVal) > 0)
        e.HasSig = true;
    }
    g_Formats.Add(e);
  }
  return !g_Formats.IsEmpty();
}

// 归档扩展名（小写，不含点）；没有扩展名返回空
static UString ArchiveExt(const FString &path)
{
  UString s = fs2us(path);
  int dot = s.ReverseFind_Dot();
  if (dot < 0 || dot < s.ReverseFind_PathSepar())
    return UString();
  UString e = s.Ptr(dot + 1);
  e.MakeLower_Ascii();
  return e;
}

// 扩展名列表（空格分隔）里有没有这个扩展名，整词相等才算
static bool ExtListHas(const UString &list, const UString &ext)
{
  if (ext.IsEmpty())
    return false;
  unsigned i = 0;
  while (i < list.Len())
  {
    while (i < list.Len() && list[i] == L' ')
      i++;
    const unsigned s = i;
    while (i < list.Len() && list[i] != L' ')
      i++;
    if (i - s == ext.Len())
    {
      unsigned k = 0;
      while (k < ext.Len() && list[s + k] == ext[k])
        k++;
      if (k == ext.Len())
        return true;
    }
  }
  return false;
}

// 扩展名对得上的格式先试，其余再兜一遍
static HRESULT OpenArchive(IInStream *inStream, const UInt64 *scanSize,
    IArchiveOpenCallback *openCb, const FString &arcFs, CMyComPtr<IInArchive> &archive)
{
  if (!LoadFormats())
    return E_NOTIMPL;

  const UString ext = ArchiveExt(arcFs);
  HRESULT last = S_FALSE;
  CMyComPtr<IInArchive> emptyOk;      // 「打开了但里面 0 项」的候选，兜底用

  for (int pass = 0; pass < 2; pass++)
  {
    for (unsigned i = 0; i < g_Formats.Size(); i++)
    {
      const CFormatEntry &fe = g_Formats[i];
      const bool prefer = !ext.IsEmpty() && fe.Ext.Find(ext) >= 0;
      if (prefer != (pass == 0))
        continue;
      if (pass != 0 && !fe.HasSig)      // 没有文件头的格式只在扩展名对上时使用
        continue;

      inStream->Seek(0, STREAM_SEEK_SET, NULL);     // 每个 handler 都从文件头开始
      CMyComPtr<IInArchive> a;
      if (!g_CreateObject || g_CreateObject(&fe.Clsid, &IID_IInArchive, (void **)&a) != S_OK || !a)
        continue;
      HRESULT h = a->Open(inStream, scanSize, openCb);
      if (h != S_OK)
      {
        last = h;
        continue;
      }
      // 有些 handler 对任何垃圾字节都会返回 S_OK，靠「有没有条目」把它筛掉
      UInt32 nItems = 0;
      a->GetNumberOfItems(&nItems);
      if (nItems > 0)
      {
        archive = a;
        return S_OK;
      }
      if (!emptyOk)
        emptyOk = a;
    }
  }

  if (emptyOk)                        // 真·空归档
  {
    archive = emptyOk;
    return S_OK;
  }
  return last;
}

// 递归收集：diskPath = 磁盘真实路径, arcName = 归档内名字(用 '/')
// addSelf=false 时只收目录里的内容，自己不进归档（用于「不保留最外层目录」）
static HRESULT CollectItems(const FString &diskPath, const UString &arcName,
    CObjectVector<CDirItem> &items, FStringVector &failed, bool addSelf = true)
{
  NFind::CFileInfo fi;
  if (!fi.Find(diskPath))
  {
    failed.Add(diskPath);
    return S_FALSE;
  }

  if (addSelf)
  {
    CDirItem di(fi);
    di.Path_For_Handler = arcName;
    di.FullPath = diskPath;
    items.Add(di);
  }

  if (!fi.IsDir())
    return S_OK;

  FString dirMask = diskPath;
  if (dirMask.IsEmpty() || (dirMask.Back() != FTEXT('\\') && dirMask.Back() != FTEXT('/')))
    dirMask += FTEXT("\\");
  dirMask += FTEXT("*");

  NFind::CFindFile finder;
  NFind::CFileInfo subInfo;
  if (!finder.FindFirst(dirMask, subInfo))
    return S_OK;

  do
  {
    if (subInfo.IsDots())
      continue;
    FString childDisk = diskPath;
    if (childDisk.IsEmpty() || (childDisk.Back() != FTEXT('\\') && childDisk.Back() != FTEXT('/')))
      childDisk += FTEXT("\\");
    childDisk += subInfo.Name;

    UString childArc = arcName;
    if (!childArc.IsEmpty())
      childArc.Add_PathSepar();
    childArc += fs2us(subInfo.Name);

    CollectItems(childDisk, childArc, items, failed);
  }
  while (finder.FindNext(subInfo));

  return S_OK;
}

static const wchar_t *MethodName(int m)
{
  switch (m)
  {
    case 1: return L"zstd";
    case 2: return L"brotli";
    case 3: return L"lz4";
    case 4: return L"lz5";
    case 5: return L"lizard";
    case 6: return L"copy";
    default: return L"lzma2";
  }
}

// ============================== 压缩 ========================================
// ---------------------- 压缩主体（不显示进度） ------------------------------
static int PackCore(const wchar_t *archivePath,
    const wchar_t *srcPath, const PACK_OPTS *opts)
{
  ::InterlockedExchange(&g_CancelReq, 0);   // 每次调用都复位，避免上一次取消影响下一次
  g_Err.Empty();
  if (!archivePath || !srcPath)
  {
    SetErr(L"参数为空");
    return PK_E_PARAM;
  }
  if (!Load7zModule())
  {
    SetErr(L"加载 7z.dll 失败: 请把 7z.dll 放在本 DLL 同一目录");
    return PK_E_DLL;
  }

  FString arcFs = us2fs(archivePath);

  // 只写 7z 容器：归档名要是别的已知格式（zip/tar/gz/rar…），直接拒绝。
  // 否则会产出「名叫 zip、其实是 7z」的文件，别的解压工具打不开。
  if (LoadFormats())
  {
    const UString arcExt = ArchiveExt(arcFs);
    if (!arcExt.IsEmpty())
    {
      bool is7z = false, isOther = false;
      for (unsigned i = 0; i < g_Formats.Size(); i++)
      {
        if (!ExtListHas(g_Formats[i].Ext, arcExt))
          continue;
        if (ExtListHas(g_Formats[i].Ext, L"7z"))
          is7z = true;
        else
          isOther = true;
      }
      if (!is7z && isOther)
      {
        SetErr2(L"Pack 只写 7z 容器，归档名请改成 .7z（当前扩展名: .", arcExt + L"）");
        return PK_E_FORMAT;
      }
    }
  }

  // 源路径的结尾写法全部归一：
  //   结尾有多少个 '\' 都无所谓（"D:\dir" "D:\dir\" "D:\dir\\" 都一样，带 dir 这一层）
  //   只有显式写 "\*" 或 "\." 才是「只压目录里的内容」，归档里不带 dir 这一层
  UString srcU(srcPath);
  bool contentsOnly = false;
  for (;;)
  {
    if (srcU.IsEmpty())
      break;

    const wchar_t last = srcU.Back();
    if (last == L'\\' || last == L'/')
    {
      if (srcU.Len() <= 3 && srcU[1] == L':')      // 盘根 "D:\"，原样保留
        break;
      srcU.Delete(srcU.Len() - 1);
      continue;
    }
    if ((last == L'*' || last == L'.') && srcU.Len() >= 2)
    {
      const wchar_t prev = srcU[srcU.Len() - 2];
      if (prev == L'\\' || prev == L'/')
      {
        srcU.Delete(srcU.Len() - 1);
        contentsOnly = true;                       // "\*" 和 "\." 都是「只压内容」
        continue;
      }
    }
    break;
  }
  FString srcFs = us2fs(srcU);

  NFind::CFileInfo fi;
  if (!fi.Find(srcFs))
  {
    SetErr2(L"源路径不存在: ", srcU);
    return PK_E_SRC;
  }
  if (contentsOnly && !fi.IsDir())
  {
    SetErr2(L"\\* / \\. 只能用在目录上: ", srcU);
    return PK_E_SRC;
  }

  // 归档内的根名：源路径最后一个分量；contentsOnly 时为空（内容直接落在根上）
  UString rootName;
  if (!contentsOnly)
  {
    const int sep = srcU.ReverseFind_PathSepar();
    rootName = srcU.Ptr(sep + 1);
    if (rootName.IsEmpty())
      rootName = L"data";
  }

  CObjectVector<CDirItem> items;
  FStringVector failed;
  CollectItems(srcFs, rootName, items, failed, !contentsOnly);
  if (items.IsEmpty())
  {
    SetErr(L"没有可压缩的内容");
    return PK_E_SRC;
  }

  // 先写 .tmp，成功后原子替换 —— 关机中途断电不会毁掉旧备份
  FString tmpFs = arcFs;
  tmpFs += FTEXT(".tmp");

  HRESULT res = S_OK;
  {
    COutFileStream *outSpec = new COutFileStream;
    CMyComPtr<IOutStream> outStream = outSpec;
    if (!outSpec->Create_ALWAYS(tmpFs))
    {
      SetErr2(L"无法创建归档文件: ", UString(archivePath));
      return PK_E_DEST;
    }

    CMyComPtr<IOutArchive> outArchive;
    if (!g_CreateObject || g_CreateObject(&CLSID_Format, &IID_IOutArchive,
        (void **)&outArchive) != S_OK || !outArchive)
    {
      SetErr(L"7z.dll 未返回 IOutArchive");
      return PK_E_CREATE;
    }

    if (opts)
    {
      const wchar_t *names[6];
      NCOM::CPropVariant values[6];
      unsigned n = 0;

      names[n] = L"m";
      values[n] = MethodName(opts->Method);
      n++;

      if (opts->Level >= 0 && opts->Level <= 9)
      {
        names[n] = L"x";
        values[n] = (UInt32)opts->Level;
        n++;
      }
      if (opts->Threads > 0)
      {
        names[n] = L"mt";
        values[n] = (UInt32)opts->Threads;
        n++;
      }
      if (opts->Solid >= 0)
      {
        names[n] = L"s";
        values[n] = (opts->Solid != 0);
        n++;
      }
      if (opts->ZstdMax)
      {
        names[n] = L"max";
        values[n] = true;
        n++;
      }

      CMyComPtr<ISetProperties> setProps;
      if (outArchive->QueryInterface(IID_ISetProperties, (void **)&setProps) != S_OK || !setProps)
      {
        SetErr(L"7z.dll 不支持 ISetProperties");
        return PK_E_PROP;
      }
      HRESULT hsp = setProps->SetProperties(names, values, n);
      if (hsp != S_OK)
      {
        SetErr(L"SetProperties 被拒绝: 检查方法名/级别");
        return PK_E_PROP;
      }
    }

    CArchiveUpdateCallback *cbSpec = new CArchiveUpdateCallback;
    CMyComPtr<IArchiveUpdateCallback2> cb = cbSpec;
    cbSpec->Init(&items);
    if (opts && opts->Password && opts->Password[0])
    {
      cbSpec->PasswordIsDefined = true;
      cbSpec->Password = opts->Password;
    }

    res = outArchive->UpdateItems(outStream, items.Size(), cb);
    cbSpec->Finilize();
  }

  if (res != S_OK)
  {
    ::DeleteFileW(us2fs(tmpFs));
    if (g_CancelReq)
    {
      SetErr(L"已取消");
      return PK_E_CANCEL;
    }
    SetErr(L"UpdateItems 失败（该 7z.dll 可能不含所选方法的编码器，例如官方版没有 zstd/brotli）");
    return PK_E_UPDATE;
  }

  if (!::MoveFileExW(fs2us(tmpFs), fs2us(arcFs), MOVEFILE_REPLACE_EXISTING))
  {
    ::DeleteFileW(fs2us(tmpFs));
    SetErr(L"归档改名失败(目标被占用?)");
    return PK_E_DEST;
  }

  return failed.IsEmpty() ? PK_OK : PK_E_PARTIAL;
}

// ============================== 解压 ========================================
// ---------------------- 解压主体（不显示进度） ------------------------------
static int UnpackCore(const wchar_t *archivePath,
    const wchar_t *destDir, const wchar_t *password)
{
  ::InterlockedExchange(&g_CancelReq, 0);   // 每次调用都复位，避免上一次取消影响下一次
  g_Err.Empty();
  if (!archivePath || !destDir)
  {
    SetErr(L"参数为空");
    return PK_E_PARAM;
  }
  if (!Load7zModule())
  {
    SetErr(L"加载 7z.dll 失败: 请把 7z.dll 放在本 DLL 同一目录");
    return PK_E_DLL;
  }

  FString arcFs = us2fs(archivePath);

  CInFileStream *inSpec = new CInFileStream;
  CMyComPtr<IInStream> inStream = inSpec;
  if (!inSpec->Open(arcFs))
  {
    SetErr2(L"打不开归档: ", UString(archivePath));
    return PK_E_SRC;
  }

  CMyComPtr<IInArchive> archive;
  {
    CArchiveOpenCallback *openCbSpec = new CArchiveOpenCallback;
    CMyComPtr<IArchiveOpenCallback> openCb = openCbSpec;
    if (password && password[0])
    {
      openCbSpec->PasswordIsDefined = true;
      openCbSpec->Password = password;
    }
    const UInt64 scanSize = (UInt64)1 << 23;
    if (OpenArchive(inStream, &scanSize, openCb, arcFs, archive) != S_OK)
    {
      SetErr2(L"无法识别归档格式（文件损坏 / 7z.dll 缺该格式的解码器 / 密码错误）: ",
          UString(archivePath));
      return PK_E_OPENARC;
    }
  }

  FString destFs = us2fs(destDir);
  NDir::CreateComplexDir(destFs);

  CArchiveExtractCallback *exSpec = new CArchiveExtractCallback;
  CMyComPtr<IArchiveExtractCallback> ex = exSpec;
  exSpec->Init(archive, destFs);
  if (password && password[0])
  {
    exSpec->PasswordIsDefined = true;
    exSpec->Password = password;
  }

  HRESULT res = archive->Extract(NULL, (UInt32)(Int32)(-1), false, ex);
  if (res != S_OK)
  {
    if (g_CancelReq)
    {
      SetErr(L"已取消");
      return PK_E_CANCEL;
    }
    // 走到这里说明格式认出来了，失败基本是密码或数据本身
    wchar_t ebuf[192];
    ::wsprintfW(ebuf, L"解压失败（HRESULT 0x%08X）：密码错误/缺失，或归档内数据损坏",
        (unsigned)res);
    SetErr(ebuf);
    return PK_E_EXTRACT;
  }
  // 关键：7-Zip 对「单个文件」的失败是通过 IArchiveExtractCallback::SetOperationResult
  // 上报的，Extract() 本身依然返回 S_OK。不查这里会出现「一个文件都没解出来却报成功」
  // —— 官方版 7z.dll 缺少 zstd 解码器时就是这种情况。
  if (exSpec->NumErrors > 0)
  {
    wchar_t buf[256];
    ::wsprintfW(buf, L"解压时 %u 个文件失败（7z.dll 可能缺少该方法解码器，或归档损坏/密码错误）",
        (unsigned)exSpec->NumErrors);
    SetErr(buf);
    return PK_E_EXTRACT;
  }
  return PK_OK;
}

// ============================== 进度 =======================================
// 7-Zip 通过 IProgress::SetTotal / SetCompleted 回报进度（新版没有 SetProgress）。
// 这里只「记录」，由调用方轮询——因为打包/解包调用本身是同步阻塞的，
// 单线程的 AHK 在 DllCall 期间无法刷新界面，除非用回调或另开线程。
static volatile UInt64 g_ProgTotal = 0;
static volatile UInt64 g_ProgDone  = 0;
static volatile LONG   g_ProgCalls = 0;
static volatile LONG   g_ProgNTids = 0;
static DWORD           g_ProgTids[16];

void Ev_OnTotal(UInt64 total)
{
  g_ProgTotal = total;
  g_ProgDone = 0;
  g_ProgCalls = 0;
  g_ProgNTids = 0;
}

void Ev_OnProgress(UInt64 done)
{
  g_ProgDone = done;
  g_ProgCalls = g_ProgCalls + 1;
  DWORD tid = ::GetCurrentThreadId();
  LONG n = g_ProgNTids;
  for (LONG i = 0; i < n && i < 16; i++)
    if (g_ProgTids[i] == tid)
      return;
  if (n < 16)
  {
    g_ProgTids[n] = tid;
    g_ProgNTids = n + 1;
  }
}

// 轮询：done / total 为字节数；ntids = 出现过的回调线程数（诊断用）
DLL_EXPORT int __stdcall Progress(unsigned long long *done, unsigned long long *total,
    unsigned int *ntids, unsigned int *calls)
{
  if (done)  *done  = g_ProgDone;
  if (total) *total = g_ProgTotal;
  if (ntids) *ntids = (unsigned int)g_ProgNTids;
  if (calls) *calls = (unsigned int)g_ProgCalls;
  return 0;
}

// ============================== 错误信息 / 版本 =============================
// 统一采用「调用方提供缓冲区」的形式，不返回 DLL 内指针。
// 原因：AHK 读 DLL 返回的 wchar_t* 时会因 DllCall 卸载 DLL 而访问违规。
// 返回写入的字符数（不含结尾 0）。
static int CopyToBuf(const wchar_t *s, wchar_t *buf, int cch)
{
  if (!buf || cch <= 0)
    return 0;
  int n = 0;
  if (s)
    while (s[n] && n < cch - 1) { buf[n] = s[n]; n++; }
  buf[n] = 0;
  return n;
}

DLL_EXPORT int __stdcall LastError(wchar_t *buf, int cch)
{
  return CopyToBuf(g_Err.IsEmpty() ? L"" : (const wchar_t *)g_Err, buf, cch);
}

DLL_EXPORT int __stdcall Version(wchar_t *buf, int cch)
{
  return CopyToBuf(L"7zpack 1.0 / 7-Zip-zstd COM wrapper", buf, cch);
}

// ====================== 可选的内置进度窗口 ==================================
// 为什么放在 DLL 里：AHK 是单线程，DllCall 期间它自己的界面根本刷不了。
// 所以窗口、消息泵、刷新全部由 DLL 负责：工作线程干活，调用线程只跑消息泵。
// 窗口带 WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE，只显示，绝不吃焦点。
bool Ev_IsCancelled() { return g_CancelReq != 0; }

static const wchar_t *kUiClass = L"7zpackProgressWnd";
static const int kUiW = 430, kUiH = 136;

struct CProgWin
{
  HWND  Wnd;
  HWND  Btn;
  int   Mode;           // 0 = 压缩, 1 = 解压
  wchar_t Line1[512];
  DWORD StartTick;
};

static CProgWin g_Ui;

// 只做整数运算，避免 wsprintfW 不支持 %f 的坑
static void UiSizeStr(UInt64 v, wchar_t *buf, int cch)
{
  if (v >= 0x40000000ULL)          // >= 1 GB
  {
    UInt64 t = v * 10 / 0x40000000ULL;
    ::wsprintfW(buf, L"%u.%u GB", (unsigned)(t / 10), (unsigned)(t % 10));
  }
  else if (v >= 0x100000ULL)       // >= 1 MB
  {
    UInt64 t = v * 10 / 0x100000ULL;
    ::wsprintfW(buf, L"%u.%u MB", (unsigned)(t / 10), (unsigned)(t % 10));
  }
  else if (v >= 1024)
    ::wsprintfW(buf, L"%u KB", (unsigned)(v / 1024));
  else
    ::wsprintfW(buf, L"%u B", (unsigned)v);
}

static LRESULT CALLBACK Ui_WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_TIMER:
      ::InvalidateRect(h, NULL, FALSE);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = ::BeginPaint(h, &ps);
      RECT rc;
      ::GetClientRect(h, &rc);

      HBRUSH bg = ::CreateSolidBrush(RGB(250, 250, 250));
      ::FillRect(dc, &rc, bg);
      ::DeleteObject(bg);

      ::SetBkMode(dc, TRANSPARENT);
      HFONT oldFont = (HFONT)::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));

      // 第一行：动作 + 目标路径（过长自动省略）
      ::SetTextColor(dc, RGB(28, 28, 28));
      RECT r1 = { 16, 12, rc.right - 16, 32 };
      ::DrawTextW(dc, g_Ui.Line1, -1, &r1,
          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

      // 进度条
      UInt64 total = g_ProgTotal, done = g_ProgDone;
      int pct = 100;
      if (total > 0)
        pct = (int)(done * 100 / total);
      if (pct > 100) pct = 100;
      if (pct < 0) pct = 0;

      RECT bar = { 16, 40, rc.right - 16, 64 };
      HBRUSH frame = ::CreateSolidBrush(RGB(198, 203, 209));
      ::FillRect(dc, &bar, frame);
      ::DeleteObject(frame);

      RECT inner = bar;
      ::InflateRect(&inner, -1, -1);
      HBRUSH white = ::CreateSolidBrush(RGB(255, 255, 255));
      ::FillRect(dc, &inner, white);
      ::DeleteObject(white);

      if (pct > 0)
      {
        RECT fill = inner;
        fill.right = inner.left + (LONG)((inner.right - inner.left) * pct / 100);
        HBRUSH blue = ::CreateSolidBrush(RGB(0, 120, 215));
        ::FillRect(dc, &fill, blue);
        ::DeleteObject(blue);
      }

      wchar_t pc[32];
      ::wsprintfW(pc, L"%d%%", pct);
      ::SetTextColor(dc, (pct > 45) ? RGB(255, 255, 255) : RGB(40, 40, 40));
      ::DrawTextW(dc, pc, -1, &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

      // 第三行：已完成 / 总量 / 速度
      wchar_t s1[64], s2[64], s3[64], line3[256];
      UiSizeStr(done, s1, 64);
      UiSizeStr(total, s2, 64);
      DWORD el = ::GetTickCount() - g_Ui.StartTick;
      if (el < 100) el = 100;
      UiSizeStr((UInt64)done * 1000 / el, s3, 64);
      ::wsprintfW(line3, L"%s / %s      %s/s", s1, s2, s3);
      ::SetTextColor(dc, RGB(80, 80, 80));
      RECT r3 = { 16, 72, rc.right - 16, 92 };
      ::DrawTextW(dc, line3, -1, &r3, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

      ::SelectObject(dc, oldFont);
      ::EndPaint(h, &ps);
      return 0;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == 1 && !g_CancelReq)
      {
        ::InterlockedExchange(&g_CancelReq, 1);
        ::EnableWindow(g_Ui.Btn, FALSE);
        ::SetWindowTextW(g_Ui.Btn, L"正在取消…");
      }
      return 0;

    case WM_CLOSE:          // 点右上角也算取消，但窗口留着
      if (!g_CancelReq)
        ::InterlockedExchange(&g_CancelReq, 1);
      return 0;
  }
  return ::DefWindowProcW(h, msg, wp, lp);
}

static bool UiCreate()
{
  if (g_Ui.Wnd)
    return true;

  WNDCLASSEXW wc;
  ::ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Ui_WndProc;
  wc.hInstance = g_hInstance;
  wc.hCursor = ::LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = kUiClass;
  ::RegisterClassExW(&wc);          // 已注册会返回 0，忽略即可

  int sw = ::GetSystemMetrics(SM_CXSCREEN);
  int sh = ::GetSystemMetrics(SM_CYSCREEN);

  g_Ui.Wnd = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
      kUiClass, L"7zpack", WS_POPUP | WS_BORDER,
      (sw - kUiW) / 2, (sh - kUiH) / 2, kUiW, kUiH,
      NULL, NULL, g_hInstance, NULL);
  if (!g_Ui.Wnd)
    return false;

  g_Ui.Btn = ::CreateWindowExW(0, L"BUTTON", L"取消",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      kUiW - 104, kUiH - 42, 88, 26, g_Ui.Wnd, (HMENU)1, g_hInstance, NULL);

  ::SetTimer(g_Ui.Wnd, 1, 60, NULL);
  ::ShowWindow(g_Ui.Wnd, SW_SHOWNOACTIVATE);
  ::UpdateWindow(g_Ui.Wnd);
  return true;
}

static void UiDestroy()
{
  if (!g_Ui.Wnd)
    return;
  ::KillTimer(g_Ui.Wnd, 1);
  ::DestroyWindow(g_Ui.Wnd);
  g_Ui.Wnd = NULL;
  g_Ui.Btn = NULL;
}

// 进度窗口跑在**独立线程**：它有自己的消息队列，只 GetMessage 自己的消息，
// 绝不碰宿主进程(AHK)的消息队列，因此不会让 AHK 的窗口过程被重入。
static HANDLE g_UiReady = NULL;
static DWORD  g_UiTid = 0;
static volatile LONG g_UiStarted = 0;

static DWORD WINAPI UiThreadProc(LPVOID)
{
  g_UiTid = ::GetCurrentThreadId();
  if (UiCreate())
    ::InterlockedExchange(&g_UiStarted, 1);
  if (g_UiReady)
    ::SetEvent(g_UiReady);

  MSG m;
  while (::GetMessageW(&m, NULL, 0, 0) > 0)
  {
    ::TranslateMessage(&m);
    ::DispatchMessageW(&m);
  }
  UiDestroy();
  return 0;
}

// ---------------------- 工作线程 + 等待循环 ---------------------------------
static volatile LONG   g_RunDone = 0;
static int             g_RunResult = -1;
static int             g_RunMode = 0;
static const wchar_t  *g_RunA = NULL;
static const wchar_t  *g_RunB = NULL;
static const wchar_t  *g_RunPwd = NULL;
static const PACK_OPTS *g_RunOpts = NULL;

static DWORD WINAPI RunThread(LPVOID)
{
  int r = (g_RunMode == 0) ? PackCore(g_RunA, g_RunB, g_RunOpts)
                           : UnpackCore(g_RunA, g_RunB, g_RunPwd);
  g_RunResult = r;
  ::InterlockedExchange(&g_RunDone, 1);
  return 0;
}

static int RunWithProgress(int mode, const wchar_t *a, const wchar_t *b,
    const PACK_OPTS *opts, const wchar_t *pwd, const wchar_t *caption)
{
  ::InterlockedExchange(&g_CancelReq, 0);
  ::InterlockedExchange(&g_RunDone, 0);
  g_RunResult = -1;
  g_RunMode = mode;
  g_RunA = a;
  g_RunB = b;
  g_RunOpts = opts;
  g_RunPwd = pwd;
  g_ProgDone = 0;
  g_ProgTotal = 0;

  HANDLE th = ::CreateThread(NULL, 0, RunThread, NULL, 0, NULL);
  if (!th)                       // 起不了线程就退回同步，功能不受影响
    return (mode == 0) ? PackCore(a, b, opts) : UnpackCore(a, b, pwd);

  // 350ms 内跑完的不弹窗，免得小任务一闪而过
  DWORD w = ::WaitForSingleObject(th, 350);
  bool shown = false;

  if (w == WAIT_TIMEOUT)
  {
    const wchar_t *act = (mode == 0) ? L"正在压缩  " : L"正在解压  ";
    g_Ui.Wnd = NULL;
    g_Ui.Btn = NULL;
    g_Ui.Mode = mode;
    ::lstrcpynW(g_Ui.Line1, act, 32);
    ::lstrcpynW(g_Ui.Line1 + ::lstrlenW(g_Ui.Line1), caption ? caption : L"", 470);
    g_Ui.StartTick = ::GetTickCount();

    ::InterlockedExchange(&g_UiStarted, 0);
    g_UiTid = 0;
    g_UiReady = ::CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE uiTh = ::CreateThread(NULL, 0, UiThreadProc, NULL, 0, NULL);
    if (uiTh)
    {
      ::WaitForSingleObject(g_UiReady, 2000);
      shown = (g_UiStarted != 0);
    }
    else if (g_UiReady)
    {
      ::CloseHandle(g_UiReady);
      g_UiReady = NULL;
    }

    ::WaitForSingleObject(th, INFINITE);

    if (uiTh)
    {
      if (shown)
      {
        g_ProgDone = g_ProgTotal;
        ::Sleep(150);            // 让最后一帧(100%)露一下
      }
      ::PostThreadMessageW(g_UiTid, WM_QUIT, 0, 0);
      ::WaitForSingleObject(uiTh, 3000);
      ::CloseHandle(uiTh);
    }
    if (g_UiReady)
    {
      ::CloseHandle(g_UiReady);
      g_UiReady = NULL;
    }
  }
  else
  {
    ::WaitForSingleObject(th, INFINITE);
  }

  ::CloseHandle(th);
  return g_RunResult;
}

// ---------------------- 对外导出 --------------------------------------------
DLL_EXPORT int __stdcall Pack(const wchar_t *archivePath,
    const wchar_t *srcPath, const PACK_OPTS *opts)
{
  if (!opts || !opts->ShowProgress)
    return PackCore(archivePath, srcPath, opts);
  return RunWithProgress(0, archivePath, srcPath, opts, NULL, archivePath);
}

DLL_EXPORT int __stdcall Unpack(const wchar_t *archivePath,
    const wchar_t *destDir, const wchar_t *password)
{
  return UnpackCore(archivePath, destDir, password);
}

// showProgress != 0 时在 DLL 内显示进度窗口
DLL_EXPORT int __stdcall UnpackEx(const wchar_t *archivePath,
    const wchar_t *destDir, const wchar_t *password, int showProgress)
{
  if (!showProgress)
    return UnpackCore(archivePath, destDir, password);
  return RunWithProgress(1, archivePath, destDir, NULL, password, destDir);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    g_hInstance = hModule;
    ::DisableThreadLibraryCalls(hModule);
  }
  return TRUE;
}
