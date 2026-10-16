// Run this orchestration script in Codex's functions.exec runtime (tools + text globals).
// It uses the built-in image generator, without an API key or additional dependencies.
// Run from the repository root. Generation is stochastic; review sheet.png afterwards.
const source = await tools.exec_command({
    cmd: "cat tools/nxbox/avatars/prompts.json",
    max_output_tokens: 12000,
});
if (source.exit_code !== 0) throw new Error(source.output);
const production = JSON.parse(source.output);
const quote = (value) => "'" + value.replaceAll("'", "'\\''") + "'";
for (const [id, prompt] of Object.entries(production.prompts)) {
    if (!/^[a-z-]+$/.test(id)) throw new Error("Invalid character ID");
    const result = await tools.image_gen__imagegen({
        transparent_background: true,
        prompt,
    });
    const path = result.output_hint?.match(/ as (\/[^\n]+\.png) by default/)?.[1];
    if (!path) throw new Error(`Image generator did not return a saved PNG for ${id}`);
    const copy = await tools.exec_command({
        cmd: `cp ${quote(path)} ${quote(`tools/nxbox/avatars/${id}.png`)}`,
        max_output_tokens: 500,
    });
    if (copy.exit_code !== 0) throw new Error(copy.output);
    text(`Saved ${id}`);
}
const build = await tools.exec_command({
    cmd: "python3 tools/nxbox/generate_avatars.py",
    max_output_tokens: 1000,
});
if (build.exit_code !== 0) throw new Error(build.output);
text("Regenerated masters, embedded portraits, PC catalogue and contact sheet. Review sheet.png.");
