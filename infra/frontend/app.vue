<template>
  <NuxtLayout>
    <NuxtPage />
  </NuxtLayout>
</template>

<style>
:root {
  color-scheme: dark;
  --bg: #0f1115;
  --card: #1c1f26;
  --accent: #8ab4f8;
  --text: #e8eaed;
  --muted: #9aa0a6;
  --border: #2c2f36;
}
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: 'Inter', system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
}
a { color: var(--accent); }
.card {
  background: var(--card);
  border: 1px solid var(--border);
  border-radius: 12px;
  box-shadow: 0 8px 24px rgba(0,0,0,0.25);
}
button {
  background: #2d3038;
  color: var(--text);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 8px 12px;
  cursor: pointer;
  transition: background 0.2s, border-color 0.2s;
}
button:hover { background: #363a45; border-color: var(--accent); }
input, select {
  background: #12151b;
  color: var(--text);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 8px;
}
</style>
