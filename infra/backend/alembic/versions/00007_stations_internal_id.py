"""stations internal id

Revision ID: 00007_stations_internal_id
Revises: 00006_stations_name_text
Create Date: 2026-02-04
"""

import uuid

from alembic import op
import sqlalchemy as sa

revision = "00007"
down_revision = "00006"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.alter_column(
        "stations",
        "id",
        new_column_name="station_id",
        existing_type=sa.String(length=64),
        nullable=False,
    )
    op.add_column("stations", sa.Column("id", sa.String(length=36), nullable=True))

    conn = op.get_bind()
    rows = conn.execute(sa.text("SELECT station_id FROM stations")).fetchall()
    for (station_id,) in rows:
        conn.execute(
            sa.text("UPDATE stations SET id = :id WHERE station_id = :station_id"),
            {"id": str(uuid.uuid4()), "station_id": station_id},
        )

    op.drop_constraint("stations_pkey", "stations", type_="primary")
    op.alter_column("stations", "id", nullable=False)
    op.create_primary_key("stations_pkey", "stations", ["id"])
    op.create_index("ix_stations_station_id", "stations", ["station_id"], unique=True)


def downgrade() -> None:
    op.drop_index("ix_stations_station_id", table_name="stations")
    op.drop_constraint("stations_pkey", "stations", type_="primary")
    op.create_primary_key("stations_pkey", "stations", ["station_id"])
    op.drop_column("stations", "id")
    op.alter_column(
        "stations",
        "station_id",
        new_column_name="id",
        existing_type=sa.String(length=64),
        nullable=False,
    )
