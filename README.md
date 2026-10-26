# tree-sitter-smarty

A [tree-sitter](https://tree-sitter.github.io/) grammar for
[Smarty](https://www.smarty.net/) templates: HTML with embedded Smarty tags.
Used by [smarty-zed-support](https://github.com/elsamrafisptr/smarty-zed-support).

It is a fork of [tree-sitter-html](https://github.com/tree-sitter/tree-sitter-html)
with Smarty nodes added by the external scanner (`src/scanner.c`):

- `smarty_open` / `smarty_close`: `{` / `{{` and `}` / `}}`, tracked so single-
  and double-brace tags close correctly. The character after `{` must not be
  whitespace, so `{$x}` and `{if $x}` are tags while `{ }` is plain text.
- `smarty_comment`: opaque `{* ... *}` comments.

`<script>` and `<style>` bodies stay raw text, so JavaScript and CSS braces are
never parsed as Smarty tags.

## Development

`src/parser.c` is generated but committed, because editors compile it directly.
Regenerate it after every change to `grammar.js`:

```sh
npm install
npx tree-sitter generate
npx tree-sitter test          # test/corpus/*.txt
npx tree-sitter parse corpus/basic.smarty
```

## License

MIT, see [LICENSE](LICENSE).
