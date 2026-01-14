<template>
  <div class="auth-page">
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
definePageMeta({ layout: 'auth' })

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
