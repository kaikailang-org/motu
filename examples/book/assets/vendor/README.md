# Vendored

`codemirror.js` is CodeMirror 6 (MIT; codemirror 6.0.1, @codemirror/view 6.36.5,
@codemirror/state 6.5.2, @codemirror/lang-markdown 6.3.2, @codemirror/commands 6.8.1,
@codemirror/search 6.5.10, @codemirror/language 6.11.0), bundled from
`codemirror.entry.js` with esbuild 0.25.2:

    esbuild codemirror.entry.js --bundle --minify --format=esm --outfile=codemirror.js --legal-comments=eof
