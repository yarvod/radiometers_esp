"""add device temperature bindings

Revision ID: 00017
Revises: 00016
Create Date: 2026-05-12
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "00017"
down_revision = "00016"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "devices",
        sa.Column(
            "temp_bindings",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
    )
    op.alter_column("devices", "temp_bindings", server_default=None)


def downgrade() -> None:
    op.drop_column("devices", "temp_bindings")
