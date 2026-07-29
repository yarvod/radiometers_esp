"""add composite measurement history index

Revision ID: 00022
Revises: 00021
Create Date: 2026-07-29
"""

from alembic import op


revision = "00022"
down_revision = "00021"
branch_labels = None
depends_on = None


def upgrade() -> None:
    with op.get_context().autocommit_block():
        op.create_index(
            "ix_measurements_device_timestamp_id",
            "measurements",
            ["device_id", "timestamp", "id"],
            unique=False,
            postgresql_concurrently=True,
        )


def downgrade() -> None:
    with op.get_context().autocommit_block():
        op.drop_index(
            "ix_measurements_device_timestamp_id",
            table_name="measurements",
            postgresql_concurrently=True,
        )
