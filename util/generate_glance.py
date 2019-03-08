import os
import sys
import json
import io
from pathlib import Path
import difflib
import shutil
import glob
import tarfile
import zipfile
import stat
import subprocess
import cv2


def generate_glance(folder: str, out_folder: str):
    mesh_folder = os.path.join(folder, 'meshes')
    allobjs_json_file = os.path.join(mesh_folder, 'allObjs.json')

    for file in glob.glob(os.path.join(folder, '*.glance')):
        shutil.copy2(file, out_folder)

    # for file in glob.glob(os.path.join(mesh_folder, '*.msh.vtp')):
    #     shutil.copy2(file, out_folder)

    template_json_file = os.path.join(folder, 'state_pc_template.json')
    with open(template_json_file, mode='r', encoding='utf-8') as jf:
        template = json.load(jf)
    with open(allobjs_json_file, mode='r', encoding='utf-8') as jf:
        allobjs = json.load(jf)
    for obj in allobjs:
        template['sources'][0]['props']['name'] = obj['cellname'] + '_neurite'
        template['sources'][0]['props']['dataset']['name'] = obj['neurite']
        template['sources'][0]['props']['dataset']['url'] = '/static/data/' + obj['neurite']
        template['sources'][1]['props']['name'] = obj['cellname'] + '_soma'
        template['sources'][1]['props']['dataset']['name'] = obj['soma']
        template['sources'][1]['props']['dataset']['url'] = '/static/data/' + obj['soma']
        template['sources'][2]['props']['name'] = obj['cellname'] + '_puncta'
        template['sources'][2]['props']['dataset']['name'] = obj['puncta']
        template['sources'][2]['props']['dataset']['url'] = '/static/data/' + obj['puncta']

        template['views'][0]['camera']['position'] = obj['camera position']
        template['views'][0]['camera']['focalPoint'] = obj['camera focalPoint']

        glance_state_file = os.path.join(folder, 'state.json')
        with open(glance_state_file, mode='w', encoding='utf-8') as outfile:
            json.dump(template, outfile)

        glance_out_file = os.path.join(out_folder, obj['cellname'] + '.glance')

        with zipfile.ZipFile(glance_out_file, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(glance_state_file, 'state.json')

        # for fn in [obj['neurite'], obj['soma'], obj['puncta'], obj['root']]:
        #     shutil.copy2(os.path.join(mesh_folder, fn), out_folder)


def generate_thumbnails(folder: str, out_folder: str):
    for file in glob.glob(os.path.join(folder, '*.jpeg')):
        im = cv2.imread(file)
        fn = Path(file).stem
        cv2.imwrite(os.path.join(out_folder, fn + '.jpg'), im)

    for file in glob.glob(os.path.join(folder, 'thumbnails', '*.tif')):
        im = cv2.imread(file)
        fn = Path(file).stem
        cv2.imwrite(os.path.join(out_folder, fn + '.jpg'), im)


def convert_pdf_to_img(pdfs, imgs):
    for i in range(len(pdfs)):
        if not os.path.exists(pdfs[i]):
            print('file', pdfs[i], 'does not exist')
            continue
        subprocess.run(['gs', '-sDEVICE=png16m', '-r300', '-dTextAlphaBits=4', '-o', imgs[i] + '.png', pdfs[i]],
                       shell=False, check=True)


def generate_analysis_figures(folder: str, out_folder: str):
    mesh_folder = os.path.join(folder, 'meshes')
    allobjs_json_file = os.path.join(mesh_folder, 'allObjs.json')

    with open(allobjs_json_file, mode='r', encoding='utf-8') as jf:
        allobjs = json.load(jf)
    for obj in allobjs:
        cellname = obj['cellname']
        res_folder = os.path.join(out_folder, cellname)
        if os.path.exists(res_folder):
            continue
        if not os.path.exists(res_folder):
            os.mkdir(res_folder)

        pdf_folder = os.path.join(os.path.expanduser('~'), 'code', 'mgrasp-analysis', 'pv_figs_1.0')

        celltype = ''
        if cellname.startswith('PV'):
            celltype = 'contra'
            if not os.path.exists(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf')):
                celltype = 'ipsi'
                if not os.path.exists(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf')):
                    continue
        else:
            celltype = 'py'
            if not os.path.exists(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf')):
                continue

        pdfs = []
        imgs = []
        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf'))
        imgs.append(os.path.join(res_folder, 'layer_overall_synapse_pattern'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig2.pdf'))
        imgs.append(os.path.join(res_folder, 'layer_sholl_analysis'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_fig1.pdf'))
        imgs.append(os.path.join(res_folder, 'overall_synapse_pattern'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_fig2.pdf'))
        imgs.append(os.path.join(res_folder, 'sholl_analysis'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_dendrogram.pdf'))
        imgs.append(os.path.join(res_folder, 'dendrogram'))

        if celltype == 'py':
            pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_basal_fig1.pdf'))
            imgs.append(os.path.join(res_folder, 'basal_pattern'))
            pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_apical_fig1.pdf'))
            imgs.append(os.path.join(res_folder, 'apical_pattern'))

        if celltype == 'py':
            celltype = 'pyr'
        pdfs.append(os.path.join(pdf_folder, 'distden', celltype + '_' + cellname + '_distden.pdf'))
        imgs.append(os.path.join(res_folder, 'distden'))

        if celltype == 'pyr':
            celltype = 'layer_Pyr'
        if celltype == 'contra':
            celltype = 'layer_contraPV'
        if celltype == 'ipsi':
            celltype = 'layer_ipsiPV'
        pdfs.append(os.path.join(pdf_folder, 'IndividualCellBranchLevelPattern',
                                 celltype + '_' + cellname + '_branch_level_pattern.pdf'))
        imgs.append(os.path.join(res_folder, 'branch_level_pattern'))

        pdfs.append(os.path.join(pdf_folder, 'IndividualCellBranchLevelPattern_length',
                                 celltype + '_' + cellname + '_branch_level_pattern.pdf'))
        imgs.append(os.path.join(res_folder, 'branch_level_pattern_length'))

        convert_pdf_to_img(pdfs, imgs)


if __name__ == "__main__":
    # generate_glance(os.path.join(os.path.expanduser('~'), 'Google Drive', 'eeum', 'raw'),
    #                 os.path.join(os.path.expanduser('~'), 'Google Drive', 'eeum', 'static', 'data'))
    # generate_thumbnails(os.path.join(os.path.expanduser('~'), 'Google Drive', 'eeum', 'raw'),
    #                     os.path.join(os.path.expanduser('~'), 'Google Drive', 'eeum', 'static', 'data'))
    generate_analysis_figures(os.path.join(os.path.expanduser('~'), 'Google Drive', 'eeum', 'raw'),
                              os.path.join(os.path.expanduser('~'), 'Google Drive', 'eeum', 'static', 'analysis'))
