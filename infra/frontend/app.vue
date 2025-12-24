<template>
  <NuxtLayout>
    <NuxtPage />
  </NuxtLayout>
</template>

<style>
:root {
  color-scheme: light;
  --bg: #f0f0f0;
  --card: #f8f9fa;
  --accent: #3498db;
  --accent-strong: #2980b9;
  --text: #1f1f1f;
  --muted: #5f6368;
  --border: #e0e0e0;
  --shadow: 0 6px 14px rgba(0, 0, 0, 0.08);
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
  box-shadow: var(--shadow);
  padding: 16px;
}
button {
  background: linear-gradient(135deg, var(--accent), var(--accent-strong));
  color: #fff;
  border: none;
  border-radius: 12px;
  padding: 10px 14px;
  cursor: pointer;
  transition: transform 0.15s ease, box-shadow 0.15s ease, filter 0.15s ease;
  box-shadow: 0 4px 12px rgba(43, 102, 246, 0.28);
}
button:hover { transform: translateY(-1px); box-shadow: 0 8px 18px rgba(43, 102, 246, 0.35); filter: brightness(1.02); }
button:active { transform: translateY(0); filter: brightness(0.98); }
input, select {
  background: #f1f3f4;
  color: var(--text);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 10px;
  outline: none;
  transition: border-color 0.15s ease, box-shadow 0.15s ease;
}
input:focus, select:focus {
  border-color: var(--accent);
  box-shadow: 0 0 0 3px rgba(26, 115, 232, 0.15);
}
</style>
