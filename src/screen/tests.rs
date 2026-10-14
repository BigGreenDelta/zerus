//! Unit tests for the screen model. Expected captures were printed by tmux
//! 3.8 (and 3.7c where noted) for the same streams; `tests/test_screen_model.py`
//! compares a large corpus against the installed tmux.

use super::*;

fn model(rows: u16, cols: u16, bytes: &[u8]) -> ScreenModel {
    let mut model = ScreenModel::new(rows, cols);
    model.process(bytes);
    model
}

fn styled(model: &ScreenModel) -> String {
    model.capture(Capture::Styled)
}

fn plain(model: &ScreenModel) -> String {
    model.capture(Capture::Plain)
}

#[test]
fn styles_are_serialized_as_tmux_writes_them() {
    let screen = model(
        3,
        20,
        b"\x1b[53mA\x1b[1;2mB\x1b[0m\x1b[38;5;1mC\x1b[31mD\x1b[4:3mE\x1b[0ma\tb\x1b[41m  \x1b[2X",
    );
    assert_eq!(plain(&screen), "ABCDEa\tb\n\n\n");
    assert_eq!(
        styled(&screen),
        "\x1b[5:3mA\x1b[1;2mB\x1b[0m\x1b[38;5;1mC\x1b[31mD\x1b[4:3mE\x1b[0ma\tb\x1b[41m    \x1b[49m\n\n\n"
    );
    assert_eq!((screen.info().cursor_x, screen.info().cursor_y), (11, 0));
}

#[test]
fn allocated_blank_cells_print_the_colour_reset() {
    // tmux allocates a quarter of an 80-column line for the first write.
    assert_eq!(
        styled(&model(2, 80, b"ab\x1b[41m\x1b[2X\x1b[0m")),
        "ab\x1b[41m  \x1b[49m\n\n"
    );
    // A full-width erase allocates the whole line; nothing follows the colour.
    assert_eq!(
        styled(&model(2, 80, b"ab\x1b[41m\x1b[K\x1b[0m")),
        "ab\x1b[41m\n\n"
    );
}

#[test]
fn state_carries_across_rows_and_trailing_spaces_are_trimmed() {
    // Lines here are allocated to the full width, so a reset follows the text.
    let screen = model(3, 10, b"\x1b[31mred  \r\nmore\x1b[0m\r\n   ");
    assert_eq!(
        styled(&screen),
        "\x1b[31mred  \x1b[39m\n\x1b[31mmore\x1b[39m\n\n"
    );
    assert_eq!(plain(&screen), "red\nmore\n\n");
}

#[test]
fn joined_text_reads_wrapped_rows_as_one_line() {
    // Output wrapped at the edge continues on the next row, its spaces kept;
    // a line feed still ends a row, and a row that only filled up does not
    // continue.
    let screen = model(
        5,
        10,
        b"hgs: cannot read it \x1b[31m(denied)\x1b[0m\r\n0123456789\r\nlast  ",
    );
    assert_eq!(
        plain(&screen),
        "hgs: canno\nt read it\n(denied)\n0123456789\nlast\n"
    );
    assert_eq!(
        screen.joined_text(),
        "hgs: cannot read it (denied)\n0123456789\nlast\n"
    );
}

#[test]
fn wide_characters_print_once_and_the_cursor_counts_cells() {
    let screen = model(2, 10, "中文xB".as_bytes());
    assert_eq!(plain(&screen), "中文xB\n\n");
    assert_eq!(screen.info().cursor_x, 6);
    // A wide character that does not fit wraps; the last column stays empty.
    assert_eq!(
        plain(&model(3, 10, "abcdefghi中".as_bytes())),
        "abcdefghi\n中\n\n"
    );
}

#[test]
fn joining_characters_stay_with_their_base() {
    let screen = model(2, 20, "e\u{301} 👨\u{200d}👩 🇺🇸|".as_bytes());
    assert_eq!(plain(&screen), "e\u{301} 👨\u{200d}👩 🇺🇸|\n\n");
    assert_eq!(screen.info().cursor_x, 8);
}

#[test]
fn tmux_37_and_38_differ_when_a_write_breaks_a_coloured_character() {
    let stream = "\x1b[41m中文\x1b[0m\x1b[1;2HZ\x1b[1;4HW".as_bytes();
    let mut new = ScreenModel::with_compat(2, 10, Compat::Tmux38);
    new.process(stream);
    assert_eq!(styled(&new), "\x1b[41m \x1b[49mZ\x1b[41m \x1b[49mW\n\n");
    let mut old = ScreenModel::with_compat(2, 10, Compat::Tmux37);
    old.process(stream);
    assert_eq!(styled(&old), " Z W\n\n");
}

#[test]
fn tmux_37_turns_the_rest_of_an_overwritten_tab_into_tabs() {
    let stream = b"\x1b[44m\x1b[K\x1b[0m\t\r\xc3\xa9";
    let mut new = ScreenModel::with_compat(2, 20, Compat::Tmux38);
    new.process(stream);
    assert_eq!(styled(&new), "\u{e9}\x1b[44m\n\n");
    let mut old = ScreenModel::with_compat(2, 20, Compat::Tmux37);
    old.process(stream);
    assert_eq!(styled(&old), "\u{e9}\x1b[44m\t\t\t\t\t\t\t\n\n");
}

#[test]
fn hyperlinks_are_kept_but_not_printed() {
    let screen = model(3, 6, b"see \x1b]8;id=x;https://example.com\x1b\\li\r\nnk");
    assert_eq!(styled(&screen), "see li\nnk\n\n");
    assert_eq!(
        screen.capture_with_links(),
        b"see \x1b]8;id=x;https://example.com\x1b\\li\x1b]8;;\x1b\\\nnk\n\n"
    );
}

#[test]
fn hyperlinks_are_bounded_like_tmux() {
    // tmux forgets the oldest link once it holds 5000 and never links a
    // longer URI than 1024 bytes, so redrawn links cannot grow without end.
    let links = |count: usize| -> Vec<u8> {
        (0..count)
            .flat_map(|i| {
                format!("\x1b]8;;https://example.com/{i}\x1b\\\x1b]8;;\x1b\\").into_bytes()
            })
            .collect()
    };
    let first = b"\x1b]8;;https://first\x1b\\A\x1b]8;;\x1b\\";
    let kept = [&first[..], &links(4998), b"B"].concat();
    assert_eq!(
        model(2, 10, &kept).capture_with_links(),
        b"\x1b]8;;https://first\x1b\\A\x1b]8;;\x1b\\B\n\n"
    );
    let forgotten = [&first[..], &links(4999), b"B"].concat();
    assert_eq!(model(2, 10, &forgotten).capture_with_links(), b"AB\n\n");
    let mut screen = model(2, 10, b"");
    for _ in 0..20 {
        screen.process(&links(1000));
    }
    // Links 1 to 20000 were made; the newest 4999 remain.
    assert!(screen.parser.link_target(15001).is_none());
    assert!(screen.parser.link_target(15002).is_some());
    assert!(screen.parser.link_target(20000).is_some());
    assert!(screen.parser.link_target(20001).is_none());
    let long = [&b"\x1b]8;;"[..], &[b'u'; 1025], b"\x1b\\A"].concat();
    assert_eq!(model(2, 10, &long).capture_with_links(), b"A\n\n");
}

#[test]
fn split_input_gives_the_same_screen() {
    let stream =
        "\x1b[1;38;2;1;2;3mbold\x1b[0m 中文 \x1b]2;title\x07\x1b[?2004h\x1b[3;1Hend".as_bytes();
    let whole = model(4, 20, stream);
    let mut pieces = ScreenModel::new(4, 20);
    for byte in stream {
        pieces.process(std::slice::from_ref(byte));
    }
    // Byte-sized chunks end every run of text, as tmux's reads would.
    assert_eq!(styled(&pieces), styled(&whole));
    assert_eq!(pieces.info(), whole.info());
}

#[test]
fn queries_are_answered_in_order_as_tmux_answers_them() {
    let mut screen = model(
        3,
        10,
        b"ab\x1b[c\x1b[0c\x1b[1c\x1b[>c\x1b[6n\x1b[?2004h\x1b[?2004$p\x1b[4$p\x1b[?25$p\x1b[?12$p\
          \x1b[?1049$p\x1b[?9999$p\x1b[18t\x1b[19t\x1b[14t\x1bP$q q\x1b\\\x1bP$qm\x1b\\\x1b[3 q\
          \x1bP$q q\x1b\\\x1b[>0q",
    );
    // tmux 3.8 (built with sixel, which adds ";4" to the first answer) also
    // answered the pixel size and its version, which the host answers.
    assert_eq!(
        screen.take_replies(),
        b"\x1b[?1;2c\x1b[?1;2c\x1b[>84;0;0c\x1b[1;3R\x1b[?2004;1$y\x1b[4;2$y\x1b[?25;1$y\
          \x1b[?12;2$y\x1b[?1049;2$y\x1b[?9999;0$y\x1b[8;3;10t\x1b[9;3;10t\x1bP1$r q0 q\x1b\\\
          \x1bP0$r\x1b\\\x1bP1$r q3 q\x1b\\"
    );
    assert!(screen.take_replies().is_empty());
}

#[test]
fn pane_facts_follow_the_modes() {
    let screen = model(4, 10, b"\x1b[?1049h\x1b[?2004h\x1b[?1h\x1b=\x1b[?1002h\x1b[?1006h\x1b[?25l\x1b[3;8r\x1b[4h\x1b[?7l\x1b[5 q");
    let info = screen.info();
    assert_eq!(
        info.tmux_line(),
        "0 0 10 4 1 1 1 1 0 1 0 0 0 1 0 1 1 0 2 3 bar 1"
    );
    assert!(info.alternate_screen && info.bracketed_paste && info.application_cursor_keys);
    assert_eq!(info.mouse, MouseTracking::Button);
    assert!(!info.cursor_visible);
    let plain_screen = model(4, 10, b"");
    assert_eq!(
        plain_screen.info().tmux_line(),
        "0 0 10 4 0 0 0 0 1 0 0 1 0 0 0 0 0 0 0 3 default 0"
    );
}

#[test]
fn resize_reflows_like_tmux() {
    // tmux counts a padding cell as one column when it reflows, so this line
    // of width 8 splits at width 9 and its first part goes to history.
    let mut screen = model(3, 10, "中文中文".as_bytes());
    screen.resize(3, 9);
    assert_eq!(plain(&screen), "文\n\n\n");
    assert_eq!((screen.info().cursor_x, screen.info().cursor_y), (2, 0));
    // Growing pulls lines back from history.
    let mut screen = model(3, 10, b"1\r\n2\r\n3\r\n4\r\n5");
    screen.resize(5, 10);
    assert_eq!(plain(&screen), "1\n2\n3\n4\n5\n");
}

fn assert_repaint_shows_the_same(screen: &ScreenModel) {
    let info = screen.info();
    let mut repainted = ScreenModel::new(info.height as u16, info.width as u16);
    repainted.process(&screen.repaint());
    assert_eq!(repainted.visual(), screen.visual());
    assert_eq!(repainted.info(), info);
}

#[test]
fn repaint_restores_screen_cursor_and_modes() {
    for stream in [
        &b"\x1b[31mred\x1b[44m bg \x1b[0m\x1b[2;3H\x1b[1;4mx\x1b[?2004h\x1b[?1000h"[..],
        "中文 e\u{301} \x1b[4:3mcurl\x1b[0m\t|tab\r\n\x1b(0lqk\x1b(B".as_bytes(),
        b"0123456789",
        b"\x1b[2;3r\x1b[3;1Hregion\x1b[?6h\x1b[2;2Hin",
        b"\x1b[38;5;114mcolour\x1b7\x1b[3;1H\x1b[0msaved\x1b[3 q",
    ] {
        assert_repaint_shows_the_same(&model(4, 10, stream));
    }
}

#[test]
fn repaint_keeps_the_main_screen_under_the_alternate_screen() {
    let screen = model(3, 12, b"\x1b[32mmain\x1b[?1049h\x1b[0m\x1b[2;2Halternate");
    assert_repaint_shows_the_same(&screen);
    let mut repainted = ScreenModel::new(3, 12);
    repainted.process(&screen.repaint());
    let mut original = screen.clone();
    original.process(b"\x1b[?1049lback");
    repainted.process(b"\x1b[?1049lback");
    assert_eq!(repainted.visual(), original.visual());
    assert_eq!(repainted.info(), original.info());
}

#[test]
fn invalid_input_never_panics() {
    let mut screen = ScreenModel::new(3, 5);
    let mut state = 0x2545_f491_4f6c_dd1du64;
    let mut bytes = Vec::new();
    for _ in 0..20_000 {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        let byte = (state >> 24) as u8;
        bytes.push(if state % 7 == 0 { 0x1b } else { byte });
    }
    for chunk in bytes.chunks(97) {
        screen.process(chunk);
        if chunk[0] % 5 == 0 {
            screen.resize(u16::from(chunk[1] % 6), u16::from(chunk[2] % 9));
        }
    }
    let _ = screen.capture(Capture::Styled);
    let _ = screen.repaint();
}

/// Random control sequences on tiny screens, with resizes, must never panic
/// (debug builds check arithmetic overflow). `SCREEN_FUZZ_STREAMS` runs more.
#[test]
fn random_sequences_never_panic() {
    let tokens: Vec<&[u8]> = vec![
        b"a",
        b"bcd",
        b" ",
        "中".as_bytes(),
        "😀".as_bytes(),
        "\u{301}".as_bytes(),
        "\u{fe0f}".as_bytes(),
        "\u{200d}".as_bytes(),
        "🇺".as_bytes(),
        "\u{1f3fd}".as_bytes(),
        "\u{1100}".as_bytes(),
        "\u{1161}".as_bytes(),
        b"\r",
        b"\n",
        b"\t",
        b"\x08",
        b"\x0e",
        b"\x0f",
        b"\x1b7",
        b"\x1b8",
        b"\x1bM",
        b"\x1bD",
        b"\x1bE",
        b"\x1bH",
        b"\x1bc",
        b"\x1b#8",
        b"\x1b(0",
        b"\x1b(B",
        b"\x1b[H",
        b"\x1b[2;3H",
        b"\x1b[99;99H",
        b"\x1b[A",
        b"\x1b[5B",
        b"\x1b[3C",
        b"\x1b[9D",
        b"\x1b[K",
        b"\x1b[1K",
        b"\x1b[2K",
        b"\x1b[J",
        b"\x1b[1J",
        b"\x1b[2J",
        b"\x1b[3J",
        b"\x1b[3X",
        b"\x1b[2@",
        b"\x1b[9@",
        b"\x1b[2P",
        b"\x1b[L",
        b"\x1b[4M",
        b"\x1b[2S",
        b"\x1b[3T",
        b"\x1b[2;4r",
        b"\x1b[r",
        b"\x1b[5;2r",
        b"\x1b[?6h",
        b"\x1b[?6l",
        b"\x1b[?7l",
        b"\x1b[?7h",
        b"\x1b[4h",
        b"\x1b[4l",
        b"\x1b[?1049h",
        b"\x1b[?1049l",
        b"\x1b[?47h",
        b"\x1b[?47l",
        b"\x1b[3b",
        b"\x1b[Z",
        b"\x1b[3g",
        b"\x1b[g",
        b"\x1b[41m",
        b"\x1b[0m",
        b"\x1b[1;38;5;9m",
        b"\x1b[48;2;1;2;3m",
        b"\x1b]8;;x\x07",
        b"\x1b]8;;\x07",
        b"\x1b[s",
        b"\x1b[u",
        b"\x1b[?3h",
        b"\x1b[5d",
        b"\x1b[7G",
        b"\x1b[2E",
        b"\x1b[F",
        b"\xe4",
        b"\xb8",
    ];
    let streams = std::env::var("SCREEN_FUZZ_STREAMS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(200);
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = |n: u64| {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state % n
    };
    for _ in 0..streams {
        let mut screen = ScreenModel::new(1 + next(8) as u16, 1 + next(12) as u16);
        let mut stream = Vec::new();
        for _ in 0..300 {
            if next(40) == 0 {
                screen.process(&stream);
                stream.clear();
                screen.resize(1 + next(8) as u16, 1 + next(12) as u16);
            } else {
                stream.extend_from_slice(tokens[next(tokens.len() as u64) as usize]);
            }
        }
        screen.process(&stream);
        let _ = screen.capture(Capture::Styled);
        let _ = screen.capture_with_links();
        let info = screen.info();
        let mut repainted = ScreenModel::new(info.height as u16, info.width as u16);
        repainted.process(&screen.repaint());
        let _ = repainted.visual();
    }
}
