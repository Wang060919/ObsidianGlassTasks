export function noteUrl(vault, file) {
  return `obsidian://open?vault=${encodeURIComponent(vault)}&file=${encodeURIComponent(file)}`;
}

export function createMarkdown(markdownit, vault, noteFile) {
  const md = markdownit({ html: false, linkify: false, breaks: true, typographer: false });
  const originalValidate = md.validateLink;
  md.validateLink = value => originalValidate(value) && !/^(?!https?:|obsidian:)[a-z][a-z0-9+.-]*:/i.test(value);
  md.inline.ruler.before('link', 'wikilink', (state, silent) => {
    if (state.src.slice(state.pos, state.pos + 2) !== '[[' || state.linkLevel > 0) return false;
    const end = state.src.indexOf(']]', state.pos + 2);
    if (end < 0) return false;
    const inner = state.src.slice(state.pos + 2, end);
    if (!inner || /[\r\n]/.test(inner)) return false;
    const pipe = inner.indexOf('|');
    const target = (pipe < 0 ? inner : inner.slice(0, pipe)).trim();
    const file = target.startsWith('#') ? noteFile + target : target;
    if (!file) return false;
    if (!silent) {
      const open = state.push('link_open', 'a', 1);
      open.attrSet('href', noteUrl(vault, file));
      open.attrSet('class', 'note-link');
      const label = state.push('text', '', 0);
      label.content = pipe < 0 ? target.split('/').at(-1) : inner.slice(pipe + 1);
      state.push('link_close', 'a', -1);
    }
    state.pos = end + 2;
    return true;
  });
  md.core.ruler.after('inline', 'hide-task-tag', state => {
    for (const block of state.tokens) {
      let linkDepth = 0;
      for (const token of block.children || []) {
        if (token.type === 'link_open') linkDepth++;
        if (token.type === 'text' && linkDepth === 0) {
          token.content = token.content.replace(/(^|\s)#todo(?=\s|$)/g, '$1');
        }
        if (token.type === 'link_close') linkDepth--;
      }
    }
  });
  // Images never load remote content into the privileged Rainmeter page.
  md.renderer.rules.image = (tokens, index) => md.utils.escapeHtml(tokens[index].content);
  const renderLink = md.renderer.rules.link_open || ((tokens, index, options, env, self) => self.renderToken(tokens, index, options));
  md.renderer.rules.link_open = (tokens, index, options, env, self) => {
    const token = tokens[index];
    const href = token.attrGet('href');
    if (href && !/^https?:|^obsidian:/i.test(href)) {
      const file = decodeURIComponent(href);
      const parent = noteFile.includes('/') ? noteFile.slice(0, noteFile.lastIndexOf('/') + 1) : '';
      const resolved = file.startsWith('#') ? noteFile + file : parent + file;
      token.attrSet('href', noteUrl(vault, resolved));
    }
    token.attrSet('rel', 'noreferrer');
    return renderLink(tokens, index, options, env, self);
  };
  return md;
}

export function extractTasks(source, md) {
  const lines = source.replace(/^\uFEFF/, '').split(/\r?\n/);
  const tasks = [];
  for (const token of md.parse(source, {})) {
    if (token.type !== 'inline' || !token.map) continue;
    const lineNumber = token.map[0];
    const line = lines[lineNumber];
    const match = /^(\s*(?:[-*+]|\d+[.)])\s+\[)([ xX])\](?:[ \t]+|$)/.exec(line);
    // Token maps exclude fenced code and prose that merely resembles a task.
    if (!match || match[2] !== ' ' || !/^\[ \](?:\s|$)/.test(token.content)) continue;
    const body = token.content.replace(/^\[ \][ \t]*/, '');
    tasks.push({ lineNumber, line, body, html: md.renderInline(body) });
  }
  return tasks;
}

export function requestHex(value) {
  return Array.from(new TextEncoder().encode(JSON.stringify(value)), byte => byte.toString(16).padStart(2, '0')).join('');
}
