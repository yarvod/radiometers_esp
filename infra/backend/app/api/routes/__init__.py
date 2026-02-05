from .auth import router as auth_router
from .devices import router as devices_router
from .measurements import router as measurements_router
from .soundings import router as soundings_router, station_router as station_soundings_router
from .stations import router as stations_router
from .users import router as users_router

__all__ = [
    "auth_router",
    "devices_router",
    "measurements_router",
    "soundings_router",
    "station_soundings_router",
    "stations_router",
    "users_router",
]
