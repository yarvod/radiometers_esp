from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, status
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import UserCreateRequest, UserOut, UserUpdateRequest
from app.domain.entities import User
from app.services.users import UserService

router = APIRouter(prefix="/users", tags=["users"])


@router.get("", response_model=list[UserOut])
@inject
async def list_users(
    current_user: User = Depends(get_current_user),
    users: FromDishka[UserService],
):
    result = await users.list()
    return [UserOut.model_validate(user, from_attributes=True) for user in result]


@router.post("", response_model=UserOut)
@inject
async def create_user(
    payload: UserCreateRequest,
    current_user: User = Depends(get_current_user),
    users: FromDishka[UserService],
):
    user = await users.create(payload.username, payload.password)
    return UserOut.model_validate(user, from_attributes=True)


@router.patch("/{user_id}", response_model=UserOut)
@inject
async def update_user(
    user_id: str,
    payload: UserUpdateRequest,
    current_user: User = Depends(get_current_user),
    users: FromDishka[UserService],
):
    try:
        user = await users.update(user_id, payload.username, payload.password)
    except ValueError:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    return UserOut.model_validate(user, from_attributes=True)


@router.delete("/{user_id}")
@inject
async def delete_user(
    user_id: str,
    current_user: User = Depends(get_current_user),
    users: FromDishka[UserService],
):
    await users.delete(user_id)
    return {"status": "ok"}
