vv — portable Windows package
=============================

Nothing to install: run vvg.exe (the viewer window) or vv.exe (the
command-line tool) from this folder. Everything they need is in it.

  vvg.exe   double-click it, or drop a file on it, or "vvg.exe file.parquet"
  vv.exe    in a terminal: "vv.exe --help"; add this folder to your PATH to
            type just "vv" (Settings > System > About > Advanced system
            settings > Environment Variables > Path > New)

The files are not code-signed. If Windows says "Windows protected your PC",
choose More info > Run anyway; or, before extracting, right-click the zip,
Properties > Unblock. Check the zip against SHA256SUMS on the release page:
  Get-FileHash vv-*-windows-x86_64.zip

vvg keeps its settings in the registry (HKCU\Software\vv) and its cache in
%LOCALAPPDATA%\vv, as the installed (MSI) version does. To remove vv, delete
this folder (and those two, if you like).

https://github.com/balwierz/vv
