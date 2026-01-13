<template>
  <div class="page">
    <div class="card auth-card">
      <div class="card-head">
        <h2>{{ heading }}</h2>
        <span class="badge" :class="mode === 'signup' ? 'accent' : 'success'">
          {{ mode === 'signup' ? 'Создать аккаунт' : 'Вход' }}
        </span>
      </div>
      <p class="muted" v-if="mode === 'signup'">
        В системе пока нет аккаунтов — создайте первый.
      </p>
      <p class="muted" v-else>
        Войдите, чтобы управлять устройствами и архивом измерений.
      </p>

      <div class="form-group">
        <label>Логин</label>
        <input type="text" v-model="form.username" placeholder="operator" />
      </div>
      <div class="form-group">
        <label>Пароль</label>
        <input type="password" v-model="form.password" placeholder="••••••" />
      </div>

      <div class="actions">
        <button class="btn primary" @click="submit" :disabled="loading">
          {{ mode === 'signup' ? 'Создать аккаунт' : 'Войти' }}
        </button>
      </div>
      <p class="muted" v-if="statusMessage">{{ statusMessage }}</p>
    </div>
  </div>
</template>

<script setup lang="ts">
const { apiFetch, token } = useApi()

const form = reactive({ username: '', password: '' })
const mode = ref<'login' | 'signup'>('login')
const loading = ref(false)
const statusMessage = ref('')

const heading = computed(() => (mode.value === 'signup' ? 'Первый аккаунт' : 'Авторизация'))

async function loadStatus() {
  try {
    const status = await apiFetch<{ has_users: boolean }>(`/api/auth/status`)
    mode.value = status.has_users ? 'login' : 'signup'
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось проверить статус'
  }
}

async function submit() {
  if (!form.username || !form.password) {
    statusMessage.value = 'Введите логин и пароль'
    return
  }
  loading.value = true
  statusMessage.value = ''
  try {
    const endpoint = mode.value === 'signup' ? '/api/auth/signup' : '/api/auth/login'
    const res = await apiFetch<{ access_token: string }>(endpoint, {
      method: 'POST',
      body: { ...form },
    })
    token.value = res.access_token
    await navigateTo('/')
  } catch (e: any) {
    statusMessage.value = e?.data?.detail || e?.message || 'Ошибка авторизации'
  } finally {
    loading.value = false
  }
}

onMounted(loadStatus)
</script>

<style scoped>
.page { min-height: 70vh; display: flex; align-items: center; justify-content: center; padding: 24px; }
.auth-card { width: min(420px, 100%); gap: 12px; }
.card-head { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.badge { padding: 4px 10px; border-radius: 999px; background: #ecf0f1; font-weight: 700; font-size: 12px; color: #4a5568; }
.badge.success { background: #e6f8ed; color: #2e8b57; }
.badge.accent { background: #e8f4fd; color: #1f2d3d; }
.form-group { display: flex; flex-direction: column; gap: 6px; }
.actions { display: flex; gap: 10px; }
.muted { color: var(--muted); font-size: 13px; }
</style>
