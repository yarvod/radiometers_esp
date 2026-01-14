"""add device config fields

Revision ID: 00002_device_config
Revises: 00001_init
Create Date: 2026-01-13
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "00002"
down_revision = "00001"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "devices",
        sa.Column(
            "temp_labels",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'[]'::jsonb"),
        ),
    )
    op.add_column(
        "devices",
        sa.Column(
            "temp_address_labels",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
    )
    op.add_column(
        "devices",
        sa.Column(
            "adc_labels",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
    )
    op.alter_column("devices", "temp_labels", server_default=None)
    op.alter_column("devices", "temp_address_labels", server_default=None)
    op.alter_column("devices", "adc_labels", server_default=None)


def downgrade() -> None:
    op.drop_column("devices", "adc_labels")
    op.drop_column("devices", "temp_address_labels")
    op.drop_column("devices", "temp_labels")
