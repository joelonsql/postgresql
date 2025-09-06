\df+ json_schema_generate

CREATE TABLE orders
(
    id BIGINT NOT NULL GENERATED ALWAYS AS IDENTITY,
    status TEXT NOT NULL,
    PRIMARY KEY (ID)
);

CREATE TABLE order_items
(
    id BIGINT NOT NULL GENERATED ALWAYS AS IDENTITY,
    order_id BIGINT NOT NULL,
    description TEXT,
    amount NUMERIC NOT NULL,
    currency CHAR(3) NOT NULL,
    PRIMARY KEY (id),
    FOREIGN KEY (order_id) REFERENCES orders (id)
);

CREATE OR REPLACE FUNCTION get_order(order_id BIGINT) RETURNS JSONB
BEGIN ATOMIC
    SELECT jsonb_build_object
    (
        'id',
        orders.id,
        'status',
        orders.status,
        'items',
        jsonb_agg
        (
            jsonb_build_object
            (
                'description',
                order_items.description,
                'amount',
                order_items.amount,
                'currency',
                order_items.currency
            )
        )
    )
    FROM orders
    JOIN order_items
      ON order_items.order_id = orders.id
    GROUP BY orders.id;
END;

INSERT INTO orders (status) VALUES ('done');
INSERT INTO order_items (order_id, description, amount, currency) VALUES (1, 'Tomato', 1.23, 'EUR');
INSERT INTO order_items (order_id, description, amount, currency) VALUES (1, 'Banana', 2.34, 'EUR');

\t
\a
SELECT jsonb_pretty(json_schema_generate(oid)) FROM pg_proc WHERE proname = 'get_order';
