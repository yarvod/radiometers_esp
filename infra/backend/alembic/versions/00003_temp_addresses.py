"""add temp addresses list

Revision ID: 00003_temp_addresses
Revises: 00002_device_config
Create Date: 2026-01-13
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "00003_temp_addresses"
down_revision = "00002_device_config"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "devices",
        sa.Column(
            "temp_addresses",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'[]'::jsonb"),
        ),
    )
    op.alter_column("devices", "temp_addresses", server_default=None)
    op.drop_column("devices", "temp_address_labels")


def downgrade() -> None:
    op.add_column(
        "devices",
        sa.Column(
            "temp_address_labels",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
    )
    op.alter_column("devices", "temp_address_labels", server_default=None)
    op.drop_column("devices", "temp_addresses")
