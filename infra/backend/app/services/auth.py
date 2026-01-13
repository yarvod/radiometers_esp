from __future__ import annotations

import secrets
from datetime import datetime, timezone

from passlib.context import CryptContext

from app.domain.entities import AccessToken, User
from app.repositories.interfaces import TokenRepository, UserRepository

pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")


class AuthService:
    def __init__(self, users: UserRepository, tokens: TokenRepository) -> None:
        self._users = users
        self._tokens = tokens

    async def has_users(self) -> bool:
        return (await self._users.count()) > 0

    async def signup(self, username: str, password: str) -> AccessToken:
        if await self._users.count() > 0:
            raise ValueError("signup_disabled")
        password_hash = pwd_context.hash(password)
        user = await self._users.create(username=username, password_hash=password_hash)
        return await self._issue_token(user.id)

    async def login(self, username: str, password: str) -> AccessToken:
        user = await self._users.get_by_username(username)
        if not user or not pwd_context.verify(password, user.password_hash):
            raise ValueError("invalid_credentials")
        return await self._issue_token(user.id)

    async def _issue_token(self, user_id: str) -> AccessToken:
        token = secrets.token_urlsafe(32)
        return await self._tokens.create(token=token, user_id=user_id)

    async def authenticate(self, token: str | None) -> User | None:
        if not token:
            return None
        token_entry = await self._tokens.get(token)
        if not token_entry:
            return None
        return await self._users.get(token_entry.user_id)

    async def logout(self, token: str | None) -> None:
        if token:
            await self._tokens.delete(token)
