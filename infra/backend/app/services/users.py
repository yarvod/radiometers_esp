from __future__ import annotations

from typing import Sequence

from passlib.context import CryptContext

from app.domain.entities import User
from app.repositories.interfaces import UserRepository

pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")


class UserService:
    def __init__(self, users: UserRepository) -> None:
        self._users = users

    async def list(self) -> Sequence[User]:
        return await self._users.list()

    async def create(self, username: str, password: str) -> User:
        password_hash = pwd_context.hash(password)
        return await self._users.create(username=username, password_hash=password_hash)

    async def update(self, user_id: str, username: str | None, password: str | None) -> User:
        password_hash = pwd_context.hash(password) if password else None
        return await self._users.update(user_id=user_id, username=username, password_hash=password_hash)

    async def delete(self, user_id: str) -> None:
        await self._users.delete(user_id=user_id)
