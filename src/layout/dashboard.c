#include "third_party/nob/nob.h"
#include "dashboard.h"
#include "src/layout/header.h"
#include "src/layout/footer.h"
#include "../../src/dashboard/dashboard.h"

static void render_dashboard_page(String_Builder *sb, Dashboard_Data *ws) {
#include "src/i18n/i18n.h"
#include "../../build/h_to_html/layout/dashboard.h"
}

void serve_dashboard(Serve_Context *sc) {
    String_Builder *sb = &sc->body;

    // Phase 14: one template, three role centers (see src/dashboard/).
    Dashboard_Data ws = {0};
    dashboard_load(&ws, auth_current_user());

    render_page_header(sb, "Dashboard", "/dashboard");
    render_dashboard_page(sb, &ws);
    render_page_footer(sb);

    http_render_response(sc, 200, "text/html", sb_to_sv(sc->body));
}
