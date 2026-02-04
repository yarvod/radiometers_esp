"""widen stations name

Revision ID: 00006_stations_name_text
Revises: 00005_stations
Create Date: 2026-02-04
"""

from alembic import op
import sqlalchemy as sa

revision = "00006"
down_revision = "00005"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.alter_column(
        "stations",
        "name",
        existing_type=sa.String(length=128),
        type_=sa.Text(),
        existing_nullable=True,
    )


def downgrade() -> None:
    op.alter_column(
        "stations",
        "name",
        existing_type=sa.Text(),
        type_=sa.String(length=128),
        existing_nullable=True,
    )
