# Recorded terminal streams

`tests/test_screen_model.py` feeds every `NAME.COLSxROWS.bin` here to the screen
model and to an isolated tmux and compares captures, pane facts and query
replies byte for byte.

The `conpty-*` files are raw pseudoconsole output recorded on Windows 11 25H2
(build 26200) in spike S1 of the Windows Phase 2 design
(`docs/superpowers/specs/2026-10-13-windows-native-host-design.md`): `modes` is a
test program that sets terminal modes, colours and glyphs; `claude`, `codex` and
`kimi` are Claude Code 2.1.296, Codex 0.162.0 and Kimi Code 2.1.1 answering one
prompt from a local fake API. `inbox` is the system ConPTY, `bundled` the
Microsoft.Windows.Console.ConPTY 1.25.260930003 pair; `fc2` runs set
`FORCE_COLOR=2`. The Windows account name in recorded paths is replaced with
`user01`, which keeps every length unchanged.
