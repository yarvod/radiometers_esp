<template>
  <div class="shell">
    <aside class="sidebar" :class="{ open: sidebarOpen }">
      <div class="sidebar-head">
        <div class="brand">Radiometer</div>
        <button class="icon-btn" @click="sidebarOpen = false">✕</button>
      </div>
      <nav class="nav">
        <NuxtLink to="/" class="nav-item" :class="{ active: devicesActive }">
          <span class="dot"></span>
          <span>Девайсы</span>
        </NuxtLink>
        <NuxtLink to="/users" class="nav-item" :class="{ active: route.path.startsWith('/users') }">
          <span class="dot"></span>
          <span>Пользователи</span>
        </NuxtLink>
      </nav>
    </aside>

    <div class="content">
      <header class="topbar">
        <button class="burger" @click="sidebarOpen = true">☰</button>
        <div class="topbar-title">Админка</div>
      </header>
      <main class="main">
        <slot />
      </main>
    </div>

    <div class="backdrop" v-if="sidebarOpen" @click="sidebarOpen = false"></div>
  </div>
</template>

<script setup lang="ts">
const route = useRoute()
const sidebarOpen = ref(false)
const devicesActive = computed(() => !route.path.startsWith('/users'))
watch(
  () => route.fullPath,
  () => {
    sidebarOpen.value = false
  }
)
</script>

<style scoped>
.shell { display: flex; min-height: 100vh; background: #f4f6f8; color: #111827; position: relative; }
.sidebar { width: 240px; background: #111827; color: #f9fafb; padding: 18px 16px; display: flex; flex-direction: column; gap: 16px; transform: translateX(-100%); transition: transform 0.25s ease; position: fixed; top: 0; bottom: 0; left: 0; z-index: 30; }
.sidebar.open { transform: translateX(0); }
.sidebar-head { display: flex; align-items: center; justify-content: space-between; }
.brand { font-weight: 800; letter-spacing: 0.5px; font-size: 16px; }
.icon-btn { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.15); color: #f9fafb; border-radius: 10px; padding: 6px 10px; cursor: pointer; }
.nav { display: flex; flex-direction: column; gap: 8px; }
.nav-item { display: flex; align-items: center; gap: 10px; padding: 10px 12px; border-radius: 12px; color: #cbd5f5; text-decoration: none; font-weight: 600; transition: background 0.2s ease, color 0.2s ease; }
.nav-item .dot { width: 8px; height: 8px; border-radius: 999px; background: #475569; transition: background 0.2s ease; }
.nav-item.active, .nav-item:hover { background: rgba(99, 102, 241, 0.2); color: #e0e7ff; }
.nav-item.active .dot, .nav-item:hover .dot { background: #818cf8; }

.content { flex: 1; margin-left: 0; width: 100%; }
.topbar { height: 56px; display: flex; align-items: center; gap: 12px; padding: 0 20px; background: #ffffff; border-bottom: 1px solid #e5e7eb; position: sticky; top: 0; z-index: 20; }
.burger { background: #111827; color: #fff; border-radius: 10px; border: none; padding: 6px 10px; cursor: pointer; }
.topbar-title { font-weight: 700; color: #111827; }
.main { padding: 24px; }
.backdrop { position: fixed; inset: 0; background: rgba(15, 23, 42, 0.35); z-index: 25; }

@media (min-width: 900px) {
  .sidebar { transform: translateX(0); position: sticky; }
  .content { margin-left: 240px; }
  .burger { display: none; }
  .backdrop { display: none; }
  .icon-btn { display: none; }
}
</style>
