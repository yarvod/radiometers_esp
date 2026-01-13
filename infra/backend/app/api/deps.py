from fastapi import HTTPException, Request, status
from dishka.integrations.fastapi import FromDishka, inject

from app.core.config import Settings
from app.domain.entities import User
from app.services.auth import AuthService


def extract_token(request: Request, settings: Settings) -> str | None:
    auth_header = request.headers.get(settings.access_token_header)
    if auth_header and auth_header.lower().startswith("bearer "):
        return auth_header.split(" ", 1)[1]
    cookie = request.cookies.get(settings.access_token_cookie)
    if cookie:
        return cookie
    return None


@inject
async def get_current_user(
    request: Request,
    settings: FromDishka[Settings],
    auth_service: FromDishka[AuthService],
) -> User:
    token = extract_token(request, settings)
    user = await auth_service.authenticate(token)
    if not user:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="unauthorized")
    return user
