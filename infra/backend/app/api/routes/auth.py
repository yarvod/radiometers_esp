from fastapi import APIRouter, HTTPException, Request, status
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import extract_token
from app.api.schemas import AuthResponse, AuthStatusResponse, LoginRequest, SignupRequest, UserOut
from app.core.config import Settings
from app.services.auth import AuthService

router = APIRouter(prefix="/auth", tags=["auth"])


@router.get("/status", response_model=AuthStatusResponse)
@inject
async def status(
    request: Request,
    settings: FromDishka[Settings],
    auth: FromDishka[AuthService],
):
    token = extract_token(request, settings)
    user = await auth.authenticate(token)
    has_users = await auth.has_users()
    return AuthStatusResponse(has_users=has_users, user=UserOut.model_validate(user, from_attributes=True) if user else None)


@router.post("/signup", response_model=AuthResponse)
@inject
async def signup(payload: SignupRequest, auth: FromDishka[AuthService]):
    try:
        token = await auth.signup(payload.username, payload.password)
    except ValueError:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="signup_disabled")
    return AuthResponse(access_token=token.token)


@router.post("/login", response_model=AuthResponse)
@inject
async def login(payload: LoginRequest, auth: FromDishka[AuthService]):
    try:
        token = await auth.login(payload.username, payload.password)
    except ValueError:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid_credentials")
    return AuthResponse(access_token=token.token)


@router.post("/logout")
@inject
async def logout(request: Request, settings: FromDishka[Settings], auth: FromDishka[AuthService]):
    token = request.cookies.get(settings.access_token_cookie)
    await auth.logout(token)
    return {"status": "ok"}
