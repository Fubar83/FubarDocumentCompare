"""Etikettmallar för testgeneratorn. Alla mått i mm, origo övre vänster, text = baslinje."""

K = {"cmyk": [0, 0, 0, 1]}
WHITE = {"cmyk": [0, 0, 0, 0]}


def oat_drink():
    return {
        "name": "oat_drink",
        "size_mm": [80, 50],
        "dieline": {"shape": "rounded_rect", "radius_mm": 3},
        "elements": [
            {"id": "band", "type": "rect", "rect_mm": [0, 40, 80, 10],
             "color": {"spot": "PANTONE 347 C", "cmyk": [0.9, 0.0, 0.8, 0.1]}},
            {"id": "logo_circle", "type": "circle", "center_mm": [12, 10], "r_mm": 6,
             "color": {"cmyk": [0.0, 0.75, 0.9, 0.0]}},
            {"id": "logo_leaf", "type": "poly", "points_mm": [[9, 12], [12, 5.5], [15, 12]],
             "color": {"cmyk": [0.0, 0.1, 0.9, 0.0]}},
            {"id": "title", "type": "text", "text": "Ekologisk Havredryck", "font": "DejaVuSans-Bold",
             "size_pt": 11, "pos_mm": [21, 11], "color": K},
            {"id": "subtitle", "type": "text", "text": "Naturell · 1 liter", "font": "DejaVuSans",
             "size_pt": 7, "pos_mm": [21, 16], "color": {"cmyk": [0.6, 0.2, 0.0, 0.3]}},
            {"id": "ingr1", "type": "text", "text": "Ingredienser: vatten, havre 10 %,", "font": "DejaVuSans",
             "size_pt": 6, "pos_mm": [4, 23], "color": K},
            {"id": "ingr2", "type": "text", "text": "rapsolja, kalciumkarbonat, salt.", "font": "DejaVuSans",
             "size_pt": 6, "pos_mm": [4, 26.2], "color": K},
            {"id": "store", "type": "text", "text": "Förvaras kylt efter öppning.", "font": "DejaVuSans",
             "size_pt": 6, "pos_mm": [4, 29.4], "color": K},
            {"id": "intl", "type": "text", "text": "Овсяный напиток · Ρόφημα βρώμης", "font": "DejaVuSans",
             "size_pt": 5.5, "pos_mm": [4, 32.6], "color": K},
            {"id": "lot", "type": "variable", "text": "LOT 2410-0001", "font": "DejaVuSans",
             "size_pt": 5.5, "pos_mm": [4, 37.2], "color": K, "rect_mm": [3.5, 34.6, 22, 3.6]},
            {"id": "ean", "type": "ean13", "data": "731086500123", "rect_mm": [53, 19, 24, 17]},
            {"id": "best", "type": "text", "text": "Bäst före: se toppen", "font": "DejaVuSans-Bold",
             "size_pt": 6.5, "pos_mm": [4, 46.4], "color": WHITE},
            {"id": "eco", "type": "circle", "center_mm": [72, 45], "r_mm": 3.2, "color": WHITE},
            {"id": "varnish", "type": "varnish", "rect_mm": [5, 3, 14, 14]},
        ],
    }


def spice():
    return {
        "name": "spice",
        "size_mm": [70, 45],
        "dieline": {"shape": "rounded_rect", "radius_mm": 4},
        "elements": [
            {"id": "bg", "type": "rect", "rect_mm": [0, 0, 70, 45], "color": {"cmyk": [0.02, 0.06, 0.22, 0.0]}},
            {"id": "header", "type": "rect", "rect_mm": [0, 0, 70, 11], "color": {"cmyk": [0.1, 0.85, 0.95, 0.25]}},
            {"id": "title", "type": "text", "text": "Rökt Paprika", "font": "DejaVuSerif", "size_pt": 13,
             "pos_mm": [4, 8.2], "color": WHITE},
            {"id": "weight", "type": "text", "text": "45 g", "font": "DejaVuSans-Bold", "size_pt": 8,
             "pos_mm": [57, 7.6], "color": WHITE},
            {"id": "desc1", "type": "text", "text": "Mild och rökig. Passar till", "font": "DejaVuSansCondensed",
             "size_pt": 6.5, "pos_mm": [4, 16.5], "color": {"cmyk": [0.3, 0.6, 0.7, 0.6]}},
            {"id": "desc2", "type": "text", "text": "grytor, korv och grönsaker.", "font": "DejaVuSansCondensed",
             "size_pt": 6.5, "pos_mm": [4, 19.8], "color": {"cmyk": [0.3, 0.6, 0.7, 0.6]}},
            {"id": "origin", "type": "text", "text": "Ursprung: Spanien", "font": "DejaVuSans", "size_pt": 5.5,
             "pos_mm": [4, 24.0], "color": K},
            {"id": "pepper1", "type": "circle", "center_mm": [12, 33], "r_mm": 4.2,
             "color": {"cmyk": [0.05, 0.95, 0.9, 0.05]}},
            {"id": "pepper2", "type": "circle", "center_mm": [19, 35], "r_mm": 3.0,
             "color": {"cmyk": [0.0, 0.6, 0.95, 0.0]}},
            {"id": "stem", "type": "poly", "points_mm": [[11, 28.5], [13.5, 25.8], [14.5, 27.0], [12.2, 29.2]],
             "color": {"cmyk": [0.7, 0.1, 0.9, 0.2]}},
            {"id": "qr", "type": "qr", "data": "https://example.com/p/4410", "rect_mm": [50, 15, 16, 16]},
            {"id": "lot", "type": "variable", "text": "BF 2027-03 L0412", "font": "DejaVuSans", "size_pt": 5,
             "pos_mm": [41, 41.0], "color": K, "rect_mm": [40.5, 38.6, 26, 3.4]},
            {"id": "footer", "type": "rect", "rect_mm": [0, 43, 70, 2], "color": {"cmyk": [0.1, 0.85, 0.95, 0.25]}},
        ],
    }


TEMPLATES = {"oat_drink": oat_drink, "spice": spice}
