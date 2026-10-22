; ============================================================================
;  demo.ahk  ——  7zPack 用法演示
;
;  需要与 7zPack.ahk / 7zPack.dll / 7z.dll 放在同一目录，双击运行即可。
;  测试数据写到 %TEMP%\7zPack_demo，可随时删除。
; ============================================================================

#NoEnv
#SingleInstance Off
SetBatchLines, -1
SetWorkingDir %A_ScriptDir%

DEMO_DIR := A_Temp . "\7zPack_demo"
ARC      := DEMO_DIR . ".7z"
ARC_PWD  := DEMO_DIR . "_pwd.7z"

RunDemo()
ExitApp

; ============================================================================
;  演示流程
; ============================================================================
RunDemo()
{
    global DEMO_DIR, ARC, ARC_PWD
    log := ""

    ; ---- 准备测试数据：2 个子目录 + 4 个文件，含中文文件名 ----
    FileRemoveDir, %DEMO_DIR%, 1
    FileRemoveDir, %ARC%, 1
    FileRemoveDir, %ARC_PWD%, 1
    FileCreateDir, %DEMO_DIR%\src\sub
    FileCreateDir, %DEMO_DIR%\src\另一个目录
    WriteText(DEMO_DIR . "\src\readme.txt",     "7zPack demo`r`n")
    WriteText(DEMO_DIR . "\src\中文文件名.txt", "中文内容测试`r`n")
    WriteText(DEMO_DIR . "\src\sub\a.txt",      "sub dir`r`n")
    WriteText(DEMO_DIR . "\src\另一个目录\b.txt", "另一个目录`r`n")
    log .= "测试数据：" . DEMO_DIR . "\src`r`n`r`n"

    ; ---- 1) 压缩：源 → 归档，ZSTD 级别 9，带进度窗口 ----
    ;      最后一个参数 1 = 让 DLL 弹进度窗口（350ms 内完成的不弹）
    t := A_TickCount
    ok := 7zPack.Pack(DEMO_DIR . "\src", ARC, 1, 9, 0, 1, 0, "", 1)
    if (!ok)
        return Fail("压缩", log)
    FileGetSize, arcSize, %ARC%
    log .= "1) 压缩 OK：" . (A_TickCount - t) . " ms，" . arcSize . " 字节`r`n"

    ; ---- 2) 解压：归档 → 目录 ----
    out := DEMO_DIR . "\out"
    t := A_TickCount
    if (!7zPack.Unpack(ARC, out, "", 1))
        return Fail("解压", log)
    log .= "2) 解压 OK：" . (A_TickCount - t) . " ms`r`n"

    ; ---- 3) 校验解出来的内容与源目录一致 ----
    bad := DiffTree(DEMO_DIR . "\src", out . "\src")
    log .= "3) 内容校验：" . (bad = 0 ? "逐字节一致" : bad . " 个文件不一致") . "`r`n`r`n"

    ; ---- 4) 加密压缩 ----
    if (!7zPack.Pack(DEMO_DIR . "\src", ARC_PWD, 1, 9, 0, 1, 0, "sample-pw"))
        return Fail("加密压缩", log)
    log .= "4) 加密压缩 OK（密码 sample-pw）`r`n"

    ; ---- 5) 不给密码解压，应当失败 ----
    if (7zPack.Unpack(ARC_PWD, DEMO_DIR . "\out_nopwd"))
        log .= "5) 无密码解压：竟然成功了，这不对`r`n"
    else
        log .= "5) 无密码解压被拒绝，错误码 " . 7zPack.ErrCode()
             . "：" . 7zPack.ErrText() . "`r`n"

    ; ---- 6) 给对密码，应当成功 ----
    if (!7zPack.Unpack(ARC_PWD, DEMO_DIR . "\out_pwd", "sample-pw"))
        return Fail("带密码解压", log)
    log .= "6) 带密码解压 OK`r`n`r`n"

    ; ---- 7) 失败示例：源路径不存在 ----
    if (7zPack.Pack(DEMO_DIR . "\__not_exist__", DEMO_DIR . "\x.7z"))
        log .= "7) 不存在的源：竟然成功了，这不对`r`n"
    else
        log .= "7) 源路径不存在时被拒绝：`r`n   " . 7zPack.ErrText()

    MsgBox, 64, 7zPack demo, %log%
}

; ============================================================================
;  辅助
; ============================================================================
; 失败统一提示；用户点过「取消」时单独说明
Fail(what, log)
{
    if (7zPack.ErrCode() = 11)
        MsgBox, 64, 7zPack demo, % log . "`r`n已取消，未生成归档。"
    else
        MsgBox, 16, 7zPack demo, % log . "`r`n" . what . " 失败（错误码 "
            . 7zPack.ErrCode() . "）：`r`n  " . 7zPack.ErrText()
}

; 写一个 UTF-8 文本文件
WriteText(path, text)
{
    FileDelete, %path%
    FileAppend, %text%, *%path%
}

; 递归比较两个目录，返回不一致的文件数
DiffTree(a, b)
{
    bad := 0
    Loop, Files, %a%\*, R
    {
        if InStr(A_LoopFileAttrib, "D")
            continue
        rel := SubStr(A_LoopFileFullPath, StrLen(a) + 2)
        q := b . "\" . rel
        if (!FileExist(q))
        {
            bad++
            continue
        }
        FileGetSize, s1, % A_LoopFileFullPath
        FileGetSize, s2, %q%
        if (s1 != s2)
        {
            bad++
            continue
        }
        FileRead, d1, % A_LoopFileFullPath
        FileRead, d2, %q%
        if (d1 != d2)
            bad++
    }
    return bad
}

; ============================================================================
;  库（放最后只是为了文件好读：流程在前，实现交给 #Include）
; ============================================================================
#Include 7zPack.ahk