#ifndef POS_PRODUCT_IMAGES_H_
#define POS_PRODUCT_IMAGES_H_

#include "src/layout/master_child.h"

/* product_images: child entity of /pos/products (fk = product_id via the
 * child forms' query string). Write routes only; the Pictures tab renders
 * through the master_child engine. */
void serve_pos_product_images_create(Serve_Context *sc);
void serve_pos_product_images_update(Serve_Context *sc);
void serve_pos_product_images_soft_delete(Serve_Context *sc);
void serve_pos_product_images_restore(Serve_Context *sc);

/* The fixed kind enum (NULL-terminated): single definition consumed by the
 * server-side validator here and by the Pictures tab's .choices select in
 * src/pos/products.c. */
extern const char *pim_kind_choices[];

#endif // !POS_PRODUCT_IMAGES_H_
