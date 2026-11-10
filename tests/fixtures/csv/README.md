# CSV parser fixtures (spec Phase 0 gate)

Row numbers are 1-based records; the header is row 1. "Fields" = the expected parse, with
`expected_fields = 3` unless noted.

| File | Expected |
|---|---|
| `quoted_comma.csv` | row 2 = `11`, `American President, The (1995)`, `Comedy\|Drama\|Romance` |
| `escaped_quote.csv` | row 2 title = `"Great Performances" Cats (1998)` |
| `bom.csv` | header row 1 = `movieId`, `title`, `genres` (the BOM is skipped, not part of `movieId`) |
| `crlf.csv` | 3 records; no field ends in `\r`; row 3 title = `Jumanji, The (1995)` |
| `no_trailing_newline.csv` | see the note below |
| `embedded_newline.csv` | row 2 title = `Multi\nLine` (newline inside quotes is literal) |
| `malformed_unterminated.csv` | CsvError at row 3 (unterminated quoted field; the error names the row where the field started) |
| `malformed_field_count.csv` | CsvError at row 4 (4 fields, expected 3) |
| `malformed_stray_quote.csv` | CsvError at row 2 (quote inside an unquoted field) |
| `malformed_after_quote.csv` | CsvError at row 2 (characters after a closing quote) |

Note on `no_trailing_newline.csv`: the last record is `2,Jumanji (1995)` — only 2 fields. With
`expected_fields = 3` it must fail at row 3; with no expected count it parses as 2 fields. This
proves the final record is read even without a trailing newline.
