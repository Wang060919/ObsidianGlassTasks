import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { createMarkdown, extractTasks, noteUrl, requestHex } from '../resources/ui/tasks.mjs';
const require = createRequire(import.meta.url);
const markdownit = require('../resources/ui/vendor/markdown-it.min.js');
const md = createMarkdown(markdownit, 'FixtureVault', 'Inbox/任务收件箱.md');

test('renders emphasis, code, wiki aliases and safe links', () => {
  const html = md.renderInline('**重点** `#todo` [[Projects/中文|笔记]] [网页](https://example.com) #todo');
  assert.match(html, /<strong>重点<\/strong>/);
  assert.match(html, /<code>#todo<\/code>/);
  assert.match(html, /obsidian:\/\/open\?vault=FixtureVault&amp;file=Projects%2F/);
  assert.match(html, />笔记<\/a>/);
  assert.match(html, /href="https:\/\/example.com"/);
  assert.equal(html.match(/#todo/g).length, 1);
});
test('rejects HTML, executable protocols, and external images', () => {
  const html = md.renderInline('<img src=x onerror=alert(1)> [bad](javascript:alert(1)) ![说明](https://external.test/x.png)');
  assert.doesNotMatch(html, /<img|href="javascript:|onerror="|<script/);
  assert.match(html, /&lt;img/);
  assert.match(html, /说明/);
});
test('preserves physical lines and ignores completed tasks and fenced examples', () => {
  const text = '\uFEFF# 收件箱\r\n\r\n- [ ] **中文** #todo\r\n  换行描述\r\n- [x] 完成\r\n```md\r\n- [ ] 假任务\r\n```\r\n- 项目\r\n  - [ ] 子任务\r\n';
  const tasks = extractTasks(text, md);
  assert.equal(tasks.length, 2);
  assert.equal(tasks[0].lineNumber, 2);
  assert.equal(tasks[0].line, '- [ ] **中文** #todo');
  assert.match(tasks[0].html, /<br>\n换行描述/);
  assert.equal(tasks[1].lineNumber, 9);
});
test('wiki labels are escaped and code does not become a link', () => {
  assert.doesNotMatch(md.renderInline('`[[note]]` [[note|<img src=x>]]'), /<img/);
  assert.match(md.renderInline('`[[note]]`'), /<code>\[\[note\]\]<\/code>/);
  assert.match(md.renderInline('[[#段落|这里]]'), /file=Inbox%2F/);
});
test('relative Markdown links open in Obsidian and raw tags inside links survive', () => {
  const html = md.renderInline('[笔记](next.md) [#todo](https://example.com)');
  assert.match(html, /file=Inbox%2Fnext.md/);
  assert.match(html, />#todo<\/a>/);
});
test('request transport contains no Rainmeter or shell metacharacters', () => {
  const data = { line: '- [ ] 中文 " [!Execute] $() #@#', id: 'test' };
  const hex = requestHex(data);
  assert.match(hex, /^[0-9a-f]+$/);
  assert.deepEqual(JSON.parse(Buffer.from(hex, 'hex').toString()), data);
  assert.equal(new URL(noteUrl('FixtureVault', 'Projects/中文')).protocol, 'obsidian:');
});
