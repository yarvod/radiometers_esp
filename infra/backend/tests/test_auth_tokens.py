from datetime import datetime, timedelta, timezone
from unittest.mock import AsyncMock

import pytest

from app.core.config import Settings
from app.domain.entities import AccessToken, User
from app.services.auth import AuthService


def make_user() -> User:
    return User(
        id="user-1",
        username="operator",
        password_hash="hashed",
        created_at=datetime.now(timezone.utc),
    )


def make_token(expires_at: datetime) -> AccessToken:
    return AccessToken(
        token="token-1",
        user_id="user-1",
        created_at=datetime.now(timezone.utc),
        expires_at=expires_at,
    )


@pytest.mark.asyncio
async def test_issue_token_uses_ttl_window():
    users = AsyncMock()
    tokens = AsyncMock()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    before = datetime.now(timezone.utc)
    tokens.create.return_value = make_token(before + timedelta(minutes=720))
    await auth._issue_token("user-1")
    after = datetime.now(timezone.utc)

    assert tokens.create.await_count == 1
    _, kwargs = tokens.create.call_args
    expires_at = kwargs["expires_at"]
    assert before + timedelta(minutes=720) <= expires_at <= after + timedelta(minutes=720)


@pytest.mark.asyncio
async def test_authenticate_expired_token_deletes():
    users = AsyncMock()
    tokens = AsyncMock()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    users.get.return_value = make_user()
    tokens.get.return_value = make_token(datetime.now(timezone.utc) - timedelta(minutes=1))

    result = await auth.authenticate("token-1")

    assert result is None
    tokens.delete.assert_awaited_once_with("token-1")


@pytest.mark.asyncio
async def test_authenticate_valid_token_returns_user():
    users = AsyncMock()
    tokens = AsyncMock()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    user = make_user()
    users.get.return_value = user
    tokens.get.return_value = make_token(datetime.now(timezone.utc) + timedelta(minutes=5))

    result = await auth.authenticate("token-1")

    assert result == user
    tokens.delete.assert_not_called()


@pytest.mark.asyncio
async def test_authenticate_missing_token_returns_none():
    users = AsyncMock()
    tokens = AsyncMock()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    tokens.get.return_value = None

    result = await auth.authenticate("token-1")

    assert result is None
    users.get.assert_not_called()
    tokens.delete.assert_not_called()


@pytest.mark.asyncio
async def test_logout_deletes_token():
    users = AsyncMock()
    tokens = AsyncMock()
    settings = Settings(access_token_ttl_minutes=720)
    auth = AuthService(users, tokens, settings)

    await auth.logout("token-1")

    tokens.delete.assert_awaited_once_with("token-1")
