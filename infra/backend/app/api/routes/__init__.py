from .auth import router as auth_router
from .devices import router as devices_router
from .measurements import router as measurements_router
from .stations import router as stations_router
from .users import router as users_router

__all__ = ["auth_router", "devices_router", "measurements_router", "stations_router", "users_router"]
