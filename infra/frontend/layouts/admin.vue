<template>
  <div class="app-shell" :class="{ 'sidebar-collapsed': !sidebarOpen }">
    <aside class="sidebar" :class="{ open: sidebarOpen }">
      <div class="logo">Radiometer</div>
      <nav>
        <NuxtLink to="/" class="nav-link" :class="{ active: devicesActive }" @click="closeSidebarIfMobile">
          <span class="nav-icon" aria-hidden="true">📟</span>
          <span>Девайсы</span>
        </NuxtLink>
        <NuxtLink
          to="/stations"
          class="nav-link"
          :class="{ active: route.path.startsWith('/stations') }"
          @click="closeSidebarIfMobile"
        >
          <span class="nav-icon" aria-hidden="true">🎈</span>
          <span>Станции</span>
        </NuxtLink>
        <NuxtLink
          to="/soundings-schedule"
          class="nav-link"
          :class="{ active: route.path.startsWith('/soundings-schedule') }"
          @click="closeSidebarIfMobile"
        >
          <span class="nav-icon" aria-hidden="true">📈</span>
          <span>Профили</span>
        </NuxtLink>
        <NuxtLink
          to="/users"
          class="nav-link"
          :class="{ active: route.path.startsWith('/users') }"
          @click="closeSidebarIfMobile"
        >
          <span class="nav-icon" aria-hidden="true">👥</span>
          <span>Пользователи</span>
        </NuxtLink>
      </nav>
    </aside>

    <div class="main">
      <header class="app-header">
        <button class="menu-toggle" @click="toggleSidebar">
          <span class="menu-icon">🧭</span>
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
const devicesActive = computed(
  () =>
    !route.path.startsWith('/users') &&
    !route.path.startsWith('/stations') &&
    !route.path.startsWith('/soundings-schedule')
)
const isMobileViewport = () => (process.client ? window.innerWidth <= 960 : false)
const closeSidebarIfMobile = () => {
  if (isMobileViewport()) {
    sidebarOpen.value = false
  }
}
const toggleSidebar = () => {
  sidebarOpen.value = !sidebarOpen.value
}
watch(
  () => route.fullPath,
  () => {
    closeSidebarIfMobile()
  }
)
onMounted(() => {
  if (!isMobileViewport()) {
    sidebarOpen.value = true
  }
})
</script>
