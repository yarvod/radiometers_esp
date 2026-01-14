<template>
  <div class="app-shell" :class="{ 'sidebar-collapsed': !sidebarOpen }">
    <aside class="sidebar" :class="{ open: sidebarOpen }">
      <div class="logo">Radiometer</div>
      <nav>
        <NuxtLink to="/" class="nav-link" :class="{ active: devicesActive }">
          <span class="dot"></span>
          <span>Девайсы</span>
        </NuxtLink>
        <NuxtLink to="/users" class="nav-link" :class="{ active: route.path.startsWith('/users') }">
          <span class="dot"></span>
          <span>Пользователи</span>
        </NuxtLink>
      </nav>
    </aside>

    <div class="main">
      <header class="app-header">
        <button class="menu-toggle" @click="toggleSidebar">
          <span class="menu-icon">☰</span>
          <span class="menu-label">Меню</span>
        </button>
        <div class="topbar">
          <div class="page-title">Админка</div>
        </div>
      </header>
      <slot />
    </div>

    <button class="sidebar-backdrop" v-if="sidebarOpen" @click="sidebarOpen = false"></button>
  </div>
</template>

<script setup lang="ts">
const route = useRoute()
const sidebarOpen = ref(false)
const devicesActive = computed(() => !route.path.startsWith('/users'))
const toggleSidebar = () => {
  sidebarOpen.value = !sidebarOpen.value
}
watch(
  () => route.fullPath,
  () => {
    sidebarOpen.value = false
  }
)
onMounted(() => {
  if (window.innerWidth > 960) {
    sidebarOpen.value = true
  }
})
</script>
