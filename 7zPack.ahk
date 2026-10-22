; ============================================================================
;  7zPack.ahk  ——  7-Zip(zstd) 压缩 / 解压，AHK v1 类封装
;
;  依赖：
;        7zPack.dll（必须与 7z.dll 同目录）
;        7z.dll
;
;  用法：
;        ; 默认：7zPack.dll 与本库文件同目录，首次调用自动加载
;        if (7zPack.Pack("C:\Users\xxx\Documents", "D:\bak\docs.7z", 1, 9))
;            MsgBox 成功
;        else
;            MsgBox % 7zPack.ErrText()
;   
;        ; 也可以自定义 7zPack.dll 位置（放在别处 / 多个版本共存时）
;        7zPack.Init("D:\Tools\7zPack\7zPack.dll")
;        ; 或
;        7z := new 7zPack("D:\Tools\7zPack\7zPack.dll")
;
;  完整示例见 demo.ahk
; ============================================================================

class 7zPack
{
    static Dll    := ""      ; 空 = 用默认（与本库 .ahk 同目录）；也可由 Init() 指定
    static Handle := 0       ; LoadLibrary 句柄，0 表示未加载
    static Err    := 0       ; 最近一次的错误码，0 = 无错

    ;  Init([dllPath])  ——  加载 DLL，返回 true/false
    ;    dllPath 省略  : 用当前已设置路径；都没有则用默认路径（本文件同目录）
    ;    dllPath 传新值: 换成该路径；若与已加载的不同，会先卸载旧的再加载
    Init(dllPath := "")
    {
        if (dllPath != "")
        {
            if (7zPack.Handle && (dllPath != 7zPack.Dll))
            {
                DllCall("FreeLibrary", "Ptr", 7zPack.Handle)
                7zPack.Handle := 0
            }
            7zPack.Dll := dllPath
        }

        if (7zPack.Handle)
            return true

        if (7zPack.Dll = "")
            7zPack.Dll := 7zPack._DefaultPath()

        if (!(7zPack.Handle := DllCall("LoadLibraryW", "WStr", 7zPack.Dll, "Ptr")))
        {
            7zPack.Handle := 0
            7zPack.Err := -2
            return false
        }
        return true
    }

    ; 默认路径锚定到「本库 .ahk 文件所在目录」，而不是调用方脚本目录，
    ; 这样被别的脚本 #Include 之后也能找对地方。
    _DefaultPath()
    {
        return RegExReplace(A_LineFile, "\\[^\\]*$") . "\7zPack.dll"
    }

    __New(dllPath := "")
    {
        if (!7zPack.Init(dllPath))
            throw Exception("无法加载 7zPack.dll：" . 7zPack.Dll, -1)
    }

    ;  Pack(源, 归档, 方法:=1, 级别:=9, 线程:=0, 固实:=1, zstdMax:=0, 密码:="", 显示进度:=0)
    ;    方法    : 0=LZMA2；1=ZSTD；2=BROTLI；3=LZ4；4=LZ5；5=LIZARD；6=COPY
    ;    级别    : 0..9（-1=默认）
    ;    线程    : 0=自动
    ;    固实    : -1=默认；0=关；1=开
    ;    zstdMax : 1=用 zstd --max
    ;    进度    : 1=由 DLL 自己弹一个进度窗口（可取消）；0=静默
    ;    返回    : true=成功；false=失败（说明用 ErrText()，判断用 ErrCode()）
    Pack(src, arc, method := 1, level := 3, threads := 0, solid := 1, zstdMax := 0, pwd := "", showProgress := 0)
    {
        if (!7zPack.Init())
            return false

        VarSetCapacity(o, 32, 0)                 ; x64: 6*int + 4 填充 + ptr
        NumPut(method,  o,  0, "Int")
        NumPut(level,   o,  4, "Int")
        NumPut(threads, o,  8, "Int")
        NumPut(solid,   o, 12, "Int")
        NumPut(zstdMax, o, 16, "Int")
        NumPut(showProgress, o, 20, "Int")       ; 走的是原来的填充位，结构仍是 32 字节
        NumPut((pwd = "") ? 0 : &pwd, o, 24, "Ptr")

        ; DLL 侧签名是 (归档, 源, 选项)，这里按 AHK 习惯把源放前面，传参时换回去
        r := DllCall(7zPack.Dll . "\Pack", "WStr", arc, "WStr", src, "Ptr", &o, "Int")
        if (ErrorLevel != 0)
            r := -1

        7zPack.Err := r
        return (r = 0)
    }

    ;  Unpack(归档, 目标目录, 密码:="", 显示进度:=0)
    ;    返回 : true=成功；false=失败
    Unpack(arc, destDir, pwd := "", showProgress := 0)
    {
        if (!7zPack.Init())
            return false

        if (showProgress)
            r := DllCall(7zPack.Dll . "\UnpackEx", "WStr", arc, "WStr", destDir
                       , "WStr", (pwd = "" ? 0 : pwd), "Int", 1, "Int")
        else
            r := DllCall(7zPack.Dll . "\Unpack", "WStr", arc, "WStr", destDir
                       , "WStr", (pwd = "" ? 0 : pwd), "Int")
        if (ErrorLevel != 0)
            r := -1

        7zPack.Err := r
        return (r = 0)
    }

    ;  最近一次的错误码
    ;  只在需要「机器判断」时用（例如 11 = 用户点了取消，7 = 这个 7z.dll 没有该编码器）；
    ;  给人看的说明一律用 ErrText()，它已经是一句完整可读的话。
    ErrCode()
    {
        return 7zPack.Err
    }

    ;  最近一次失败的文字说明（写进本方法的静态缓冲区）
    ;  一句完整可读的话，直接 MsgBox 或写日志即可：DLL 每个失败分支都写了具体原因
    ;  （含涉及的文件名/路径），DLL 加载不上时这里也会给出路径。
    ErrText()
    {
        static buf := ""
        if (!7zPack.Init())
            return "无法加载 " . 7zPack.Dll . "（路径不存在、位数不匹配，或同目录缺少 7z.dll）"
        if (buf = "")
            VarSetCapacity(buf, 8192, 0)
        DllCall(7zPack.Dll . "\LastError", "Ptr", &buf, "Int", 4096, "Int")
        return StrGet(&buf)
    }
}
