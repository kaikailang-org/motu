import { EditorView, keymap, drawSelection, highlightActiveLine, placeholder } from "@codemirror/view";
import { EditorState, Compartment } from "@codemirror/state";
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { searchKeymap, highlightSelectionMatches } from "@codemirror/search";
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { syntaxHighlighting, HighlightStyle } from "@codemirror/language";
import { tags } from "@lezer/highlight";

const prose = HighlightStyle.define([
  { tag: tags.heading1, fontSize: "1.6em", fontWeight: "700" },
  { tag: tags.heading2, fontSize: "1.35em", fontWeight: "700" },
  { tag: tags.heading3, fontSize: "1.15em", fontWeight: "700" },
  { tag: [tags.heading4, tags.heading5, tags.heading6], fontWeight: "700" },
  { tag: tags.emphasis, fontStyle: "italic" },
  { tag: tags.strong, fontWeight: "700" },
  { tag: tags.strikethrough, textDecoration: "line-through" },
  { tag: tags.link, textDecoration: "underline" },
  { tag: tags.url, opacity: "0.6" },
  { tag: tags.monospace, fontFamily: "ui-monospace, Menlo, monospace", fontSize: "0.9em" },
  { tag: tags.quote, fontStyle: "italic", opacity: "0.85" },
  { tag: tags.comment, opacity: "0.5" },
  { tag: [tags.processingInstruction, tags.contentSeparator, tags.meta], opacity: "0.45" },
]);

export function createEditor(parent, { doc = "", onChange = () => {}, placeholderText = "" } = {}) {
  const wrap = new Compartment();
  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc,
      extensions: [
        history(), drawSelection(), highlightActiveLine(), highlightSelectionMatches(),
        keymap.of([indentWithTab, ...defaultKeymap, ...historyKeymap, ...searchKeymap]),
        markdown({ base: markdownLanguage }), syntaxHighlighting(prose),
        wrap.of(EditorView.lineWrapping), placeholder(placeholderText),
        EditorView.contentAttributes.of({ spellcheck: "true", autocorrect: "on", autocapitalize: "sentences" }),
        EditorView.updateListener.of((u) => { if (u.docChanged) onChange(u.state.doc.toString()); }),
      ],
    }),
  });
  return {
    view,
    get value() { return view.state.doc.toString(); },
    set value(text) { view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text } }); },
    focus() { view.focus(); },
    insert(text) { const r = view.state.selection.main; view.dispatch({ changes: { from: r.from, to: r.to, insert: text }, selection: { anchor: r.from + text.length } }); view.focus(); },
    destroy() { view.destroy(); },
  };
}
