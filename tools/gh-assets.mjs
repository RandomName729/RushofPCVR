// Lists release assets for GitHub repos: node gh-assets.mjs owner/repo[@tag] ...
for (const spec of process.argv.slice(2)) {
  const [repo, tag] = spec.split("@");
  const url = tag
    ? `https://api.github.com/repos/${repo}/releases/tags/${tag}`
    : `https://api.github.com/repos/${repo}/releases/latest`;
  const res = await fetch(url, { headers: { "User-Agent": "astrobotquest-setup" } });
  const rel = await res.json();
  console.log(`=== ${repo} ${rel.tag_name ?? rel.message} (${rel.published_at ?? ""})`);
  for (const a of rel.assets ?? []) {
    console.log(`${String(Math.round(a.size / 1048576)).padStart(6)} MB  ${a.browser_download_url}`);
  }
}
