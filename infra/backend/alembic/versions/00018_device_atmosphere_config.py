"""add device atmosphere config

Revision ID: 00018
Revises: 00017
Create Date: 2026-05-12
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "00018"
down_revision = "00017"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "devices",
        sa.Column(
            "atmosphere_config",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
    )
    op.alter_column("devices", "atmosphere_config", server_default=None)


def downgrade() -> None:
    op.drop_column("devices", "atmosphere_config")
