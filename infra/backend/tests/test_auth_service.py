import uuid
from datetime import datetime, timezone

import pytest

from app.domain.entities import AccessToken, User
from app.core.config import Settings
from app.repositories.interfaces import TokenRepository, UserRepository
from app.services.auth import AuthService


class InMemoryUserRepo(UserRepository):
    def __init__(self) -> None:
        self._users: dict[str, User] = {}

    async def list(self):
        return list(self._users.values())

    async def count(self) -> int:
        return len(self._users)

    async def get_by_username(self, username: str):
        for user in self._users.values():
            if user.username == username:
                return user
        return None

    async def get(self, user_id: str):
        return self._users.get(user_id)

    async def create(self, username: str, password_hash: str):
        user_id = str(uuid.uuid4())
        user = User(id=user_id, username=username, password_hash=password_hash, created_at=datetime.now(timezone.utc))
        self._users[user_id] = user
        return user

    async def update(self, user_id: str, username: str | None, password_hash: str | None):
        user = self._users.get(user_id)
        if not user:
            raise ValueError("User not found")
        if username is not None:
            user.username = username
        if password_hash is not None:
            user.password_hash = password_hash
        return user

    async def delete(self, user_id: str) -> None:
        self._users.pop(user_id, None)


class InMemoryTokenRepo(TokenRepository):
    def __init__(self) -> None:
        self._tokens: dict[str, AccessToken] = {}

    async def create(self, token: str, user_id: str, expires_at: datetime):
        entry = AccessToken(
            token=token,
            user_id=user_id,
            created_at=datetime.now(timezone.utc),
            expires_at=expires_at,
        )
        self._tokens[token] = entry
        return entry

    async def get(self, token: str):
        return self._tokens.get(token)

    async def delete(self, token: str) -> None:
        self._tokens.pop(token, None)


@pytest.mark.asyncio
async def test_auth_flow():
    users = InMemoryUserRepo()
    tokens = InMemoryTokenRepo()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    assert await auth.has_users() is False

    token = await auth.signup("admin", "secret")
    assert token.user_id
    assert await auth.has_users() is True

    with pytest.raises(ValueError):
        await auth.signup("other", "pass")

    login_token = await auth.login("admin", "secret")
    assert login_token.token

    user = await auth.authenticate(login_token.token)
    assert user is not None
    assert user.username == "admin"

    await auth.logout(login_token.token)
    assert await auth.authenticate(login_token.token) is None


@pytest.mark.asyncio
async def test_auth_invalid_login():
    users = InMemoryUserRepo()
    tokens = InMemoryTokenRepo()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    await auth.signup("admin", "secret")
    with pytest.raises(ValueError):
        await auth.login("admin", "wrong")
