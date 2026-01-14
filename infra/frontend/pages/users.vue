<template>
  <div class="page users-page">
    <div class="header">
      <button class="btn ghost back" @click="navigateTo('/')">← Назад</button>
      <div>
        <h2>Пользователи</h2>
        <p class="muted">Управляйте доступом к системе.</p>
      </div>
      <button class="btn primary" @click="openCreate">Добавить пользователя</button>
    </div>

    <div class="card table-card">
      <div class="card-head">
        <h3>Список пользователей</h3>
        <span class="badge success">{{ users.length }}</span>
      </div>
      <p class="muted" v-if="statusMessage">{{ statusMessage }}</p>
      <div v-if="users.length === 0" class="muted empty">Нет пользователей</div>
      <div v-else class="table-wrap">
        <table class="table">
          <thead>
            <tr>
              <th>Логин</th>
              <th>Создан</th>
              <th>Статус</th>
            </tr>
          </thead>
          <tbody>
            <tr
              v-for="user in users"
              :key="user.id"
              class="table-row"
              tabindex="0"
              @click="openEdit(user)"
              @keydown.enter="openEdit(user)"
            >
              <td>
                <strong>{{ user.username }}</strong>
                <div class="muted small">ID: {{ user.id }}</div>
              </td>
              <td>{{ formatDate(user.created_at) }}</td>
              <td><span class="chip subtle">Активен</span></td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <div v-if="createModalOpen" class="modal-backdrop" @click.self="closeCreate">
      <div class="modal card">
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
        <div class="actions">
          <button class="btn primary" @click="createUser">Создать</button>
          <button class="btn ghost" @click="closeCreate">Отмена</button>
        </div>
        <p class="muted" v-if="createStatus">{{ createStatus }}</p>
      </div>
    </div>

    <div v-if="editModalOpen" class="modal-backdrop" @click.self="closeEdit">
      <div class="modal card">
        <div class="card-head">
          <h3>Редактирование</h3>
          <span class="badge accent">{{ editForm.username }}</span>
        </div>
        <div class="form-group">
          <label>Логин</label>
          <input type="text" v-model="editForm.username" />
        </div>
        <div class="form-group">
          <label>Новый пароль</label>
          <input type="password" v-model="editForm.password" placeholder="Оставьте пустым, если не менять" />
        </div>
        <div class="actions">
          <button class="btn primary" @click="saveEdit">Сохранить</button>
          <button class="btn warning ghost" @click="confirmDelete">Удалить</button>
          <button class="btn ghost" @click="closeEdit">Отмена</button>
        </div>
        <p class="muted" v-if="editStatus">{{ editStatus }}</p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
definePageMeta({ layout: 'admin' })
useHead({ title: 'Пользователи' })

const { apiFetch } = useApi()

const users = ref<any[]>([])
const createForm = reactive({ username: '', password: '' })
const createStatus = ref('')
const statusMessage = ref('')
const createModalOpen = ref(false)
const editModalOpen = ref(false)
const editStatus = ref('')
const editForm = reactive({ id: '', username: '', password: '' })

async function loadUsers() {
  try {
    const list = await apiFetch<any[]>('/api/users')
    users.value = list
    statusMessage.value = ''
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить пользователей'
  }
}

function formatDate(value: string) {
  return new Date(value).toLocaleString()
}

function openCreate() {
  createModalOpen.value = true
  createStatus.value = ''
  createForm.username = ''
  createForm.password = ''
}

function closeCreate() {
  createModalOpen.value = false
}

function openEdit(user: any) {
  editModalOpen.value = true
  editStatus.value = ''
  editForm.id = user.id
  editForm.username = user.username
  editForm.password = ''
}

function closeEdit() {
  editModalOpen.value = false
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
    closeCreate()
    await loadUsers()
  } catch (e: any) {
    createStatus.value = e?.data?.detail || e?.message || 'Не удалось создать пользователя'
  }
}

async function saveEdit() {
  try {
    await apiFetch(`/api/users/${editForm.id}`, {
      method: 'PATCH',
      body: {
        username: editForm.username,
        password: editForm.password || undefined,
      },
    })
    closeEdit()
    await loadUsers()
  } catch (e: any) {
    editStatus.value = e?.data?.detail || e?.message || 'Не удалось обновить пользователя'
  }
}

async function confirmDelete() {
  if (!confirm(`Удалить пользователя ${editForm.username}?`)) return
  try {
    await apiFetch(`/api/users/${editForm.id}`, { method: 'DELETE' })
    closeEdit()
    await loadUsers()
  } catch (e: any) {
    editStatus.value = e?.data?.detail || e?.message || 'Не удалось удалить пользователя'
  }
}

onMounted(loadUsers)
</script>
