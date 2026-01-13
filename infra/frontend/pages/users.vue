<template>
  <div class="page">
    <div class="header">
      <button class="btn ghost" @click="navigateTo('/')">← Назад</button>
      <div>
        <h2>Пользователи</h2>
        <p class="muted">Управляйте доступом к системе.</p>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="card-head">
          <h3>Новый пользователь</h3>
          <span class="badge">Создание</span>
        </div>
        <div class="form-group">
          <label>Логин</label>
          <input type="text" v-model="createForm.username" />
        </div>
        <div class="form-group">
          <label>Пароль</label>
          <input type="password" v-model="createForm.password" />
        </div>
        <button class="btn primary" @click="createUser">Создать</button>
        <p class="muted" v-if="createStatus">{{ createStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Список</h3>
          <span class="badge success">{{ users.length }}</span>
        </div>
        <div v-if="users.length === 0" class="muted">Нет пользователей</div>
        <div v-for="user in users" :key="user.id" class="user-row">
          <div class="user-meta">
            <strong>{{ user.username }}</strong>
            <span class="muted">{{ new Date(user.created_at).toLocaleString() }}</span>
          </div>
          <div class="inline fields">
            <label class="compact">Логин
              <input type="text" v-model="user.editedName" />
            </label>
            <label class="compact">Пароль
              <input type="password" v-model="user.editedPassword" />
            </label>
          </div>
          <div class="actions">
            <button class="btn" @click="updateUser(user)">Сохранить</button>
            <button class="btn warning ghost" @click="deleteUser(user)">Удалить</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { apiFetch } = useApi()

const users = ref<any[]>([])
const createForm = reactive({ username: '', password: '' })
const createStatus = ref('')

async function loadUsers() {
  try {
    const list = await apiFetch<any[]>('/api/users')
    users.value = list.map((user) => ({
      ...user,
      editedName: user.username,
      editedPassword: '',
    }))
  } catch (e: any) {
    createStatus.value = e?.message || 'Не удалось загрузить пользователей'
  }
}

async function createUser() {
  if (!createForm.username || !createForm.password) {
    createStatus.value = 'Введите логин и пароль'
    return
  }
  createStatus.value = ''
  try {
    await apiFetch('/api/users', {
      method: 'POST',
      body: { ...createForm },
    })
    createForm.username = ''
    createForm.password = ''
    await loadUsers()
  } catch (e: any) {
    createStatus.value = e?.data?.detail || e?.message || 'Не удалось создать пользователя'
  }
}

async function updateUser(user: any) {
  try {
    await apiFetch(`/api/users/${user.id}`, {
      method: 'PATCH',
      body: {
        username: user.editedName || user.username,
        password: user.editedPassword || undefined,
      },
    })
    user.editedPassword = ''
    await loadUsers()
  } catch (e: any) {
    createStatus.value = e?.data?.detail || e?.message || 'Не удалось обновить пользователя'
  }
}

async function deleteUser(user: any) {
  if (!confirm(`Удалить пользователя ${user.username}?`)) return
  try {
    await apiFetch(`/api/users/${user.id}`, { method: 'DELETE' })
    await loadUsers()
  } catch (e: any) {
    createStatus.value = e?.data?.detail || e?.message || 'Не удалось удалить пользователя'
  }
}

onMounted(loadUsers)
</script>

<style scoped>
.page { max-width: 1100px; margin: 0 auto; padding: 24px; display: flex; flex-direction: column; gap: 16px; }
.header { display: flex; align-items: center; gap: 12px; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }
.user-row { border-top: 1px solid var(--border); padding-top: 12px; margin-top: 12px; display: flex; flex-direction: column; gap: 10px; }
.user-meta { display: flex; flex-direction: column; gap: 4px; }
.inline { display: flex; gap: 10px; flex-wrap: wrap; }
.fields > * { flex: 1 1 140px; }
.compact input { width: 100%; }
.actions { display: flex; gap: 10px; flex-wrap: wrap; }
.muted { color: var(--muted); font-size: 13px; }
</style>
