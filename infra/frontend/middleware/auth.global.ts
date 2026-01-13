export default defineNuxtRouteMiddleware((to) => {
  const token = useCookie('access_token')
  if (!token.value && to.path !== '/login') {
    return navigateTo('/login')
  }
})
