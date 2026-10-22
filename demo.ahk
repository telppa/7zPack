src := A_ComSpec                       ; 要压的东西：文件或目录都可以
arc := A_Temp . "\7zPack_demo.7z"      ; 归档
out := A_Temp . "\7zPack_demo_out"     ; 解压到哪

; 压缩：源 → 归档（默认 zstd 级别3）
7zPack.Pack(src, arc)

; 压缩：源 → 归档（指定 brotli 级别9）
7zPack.Pack(src, arc, 2, 9)

; 解压：归档 → 目录
7zPack.Unpack(arc, out)

; 失败时取细节：故意给一个不存在的源
if (!7zPack.Pack("Z:\no\such\file.txt", arc))
  MsgBox % Format("此处为报错演示`n错误码：{}`n错误信息：{}", 7zPack.ErrCode(), 7zPack.ErrText())

; 自定义 7zPack.dll 位置（7z.dll 必须与 7zPack.dll 同目录）
7zPack.Init("X:\Tools\7zPack\7zPack.dll")
7zPack.Pack(src, arc)
; 或
z := new 7zPack("Y:\Tools\7zPack\7zPack.dll")
z.Unpack(arc, out)

ExitApp

#Include 7zPack.ahk